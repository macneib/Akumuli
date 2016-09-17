/**
 * Copyright (c) 2013 Eugene Lazin <4lazin@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */


#include "storage.h"
#include "util.h"
#include "cursor.h"
#include "queryprocessor.h"

#include <cstdlib>
#include <cstdarg>
#include <stdexcept>
#include <algorithm>
#include <new>
#include <atomic>
#include <sstream>
#include <cassert>
#include <functional>
#include <sstream>

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/bind.hpp>
#include <boost/scoped_array.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/filesystem.hpp>

namespace Akumuli {

static apr_status_t create_page_file(const char* file_name, uint32_t page_index, uint32_t npages, aku_logger_cb_t logger, uint64_t page_size=0);

//----------------------------------Volume----------------------------------------------

Volume::Volume(const char* file_name,
               aku_FineTuneParams conf,
               aku_logger_cb_t logger)
    : mmap_(file_name, conf.enable_huge_tlb != 0, logger)
    , window_(conf.window_size)
    , max_cache_size_(conf.max_cache_size)
    , file_path_(file_name)
    , config_(conf)
    , logger_(logger)
    , is_temporary_ {0}
{
    mmap_.panic_if_bad();  // panic if can't mmap volume
    page_ = reinterpret_cast<PageHeader*>(mmap_.get_pointer());
    cache_.reset(new Sequencer(conf));
}

Volume::~Volume() {
    if (is_temporary_.load()) {
        mmap_.delete_file();
    }
}

PageHeader* Volume::get_page() const {
    return page_;
}

void Volume::make_readonly() {
    if (mmap_.protect_all() != AKU_SUCCESS) {
        AKU_PANIC("can't make mmap region read-only");
    }
}

void Volume::make_writable() {
    if (mmap_.unprotect_all() != AKU_SUCCESS) {
        AKU_PANIC("can't make mmap region writable");
    }
}

std::shared_ptr<Volume> Volume::safe_realloc() {
    uint32_t page_id = page_->get_page_id();
    uint32_t open_count = page_->get_open_count();
    uint32_t close_count = page_->get_close_count();
    uint32_t npages = page_->get_numpages();
    uint64_t page_size = page_->get_page_length();

    std::string new_file_name = file_path_;
                new_file_name += ".tmp";

    // this volume is temporary and should live until
    // somebody is reading its data
    mmap_.move_file(new_file_name.c_str());
    mmap_.panic_if_bad();
    is_temporary_.store(true);

    std::shared_ptr<Volume> newvol;
    auto status = create_page_file(file_path_.c_str(), page_id, npages, logger_, page_size);
    if (status != AKU_SUCCESS) {
        (*logger_)(AKU_LOG_ERROR, "Failed to create new volume");
        // Try to restore previous state on disk
        mmap_.move_file(file_path_.c_str());
        mmap_.panic_if_bad();
        AKU_PANIC("can't create new page file (out of space?)");
    }

    newvol.reset(new Volume(file_path_.c_str(), config_, logger_));
    newvol->page_->set_open_count(open_count);
    newvol->page_->set_close_count(close_count);
    return newvol;
}

void Volume::open() {
    page_->reuse();
    mmap_.flush();
}

void Volume::close() {
    page_->close();
    mmap_.flush();
}

void Volume::flush() {
    mmap_.flush();
    page_->create_checkpoint();
    mmap_.flush(0, sizeof(PageHeader));
}

//----------------------------------Storage---------------------------------------------

struct VolumeIterator {
    std::vector<std::string> volume_names;
    aku_Status               error_code;

    VolumeIterator(std::shared_ptr<MetadataStorage> db, aku_logger_cb_t logger)
        : error_code(AKU_SUCCESS)
    {
        // 1. Read configuration data
        std::string creation_time;
        try {
            db->get_configs(&creation_time);
        } catch(std::exception const& err) {
            (*logger)(AKU_LOG_ERROR, err.what());
            error_code = AKU_ENO_DATA;
            return;
        }

        // 2. Read volumes
        std::vector<MetadataStorage::VolumeDesc> volumes;
        try {
            volumes = db->get_volumes();
            if (volumes.size() == 0u) {
                throw std::runtime_error("no volumes specified");
            }
        } catch(std::exception const& err) {
            (*logger)(AKU_LOG_ERROR, err.what());
            error_code = AKU_ENO_DATA;
            return;
        }

        volume_names.resize(volumes.size());
        for(auto desc: volumes) {
            auto volume_index = desc.first;
            auto volume_path = desc.second;
            volume_names.at(volume_index) = volume_path;
        }

        // check result
        for(std::string const& path: volume_names) {
            if (path.empty()) {
                error_code = AKU_EBAD_ARG;
                (*logger)(AKU_LOG_ERROR, "invalid storage, one of the volumes is missing");
                return;
            }
        }
    }

    bool is_bad() const {
        return error_code != AKU_SUCCESS;
    }
};


static void zero_deleter(SeriesMatcher*) {}

Storage::Storage(const char* path, aku_FineTuneParams const& params)
    : config_(params)
    , open_error_code_(AKU_SUCCESS)
    , logger_(params.logger)
    , local_matcher_(&zero_deleter)
{
    // 0. Check that file exists
    auto filedesc = std::fopen(const_cast<char*>(path), "r");
    if (filedesc == nullptr) {
        // No such file
        open_error_code_ = AKU_ENOT_FOUND;
        (*logger_)(AKU_LOG_ERROR, "invalid path, no such file");
        return;
    }
    std::fclose(filedesc);

    // 1. Open db
    try {
        metadata_ = std::make_shared<MetadataStorage>(path, logger_);
    } catch(std::exception const& err) {
        (*logger_)(AKU_LOG_ERROR, err.what());
        open_error_code_ = AKU_ENOT_FOUND;
        return;
    }

    VolumeIterator v_iter(metadata_, logger_);

    if (v_iter.is_bad()) {
        open_error_code_ = v_iter.error_code;
        return;
    }

    ttl_ = config_.window_size;

    // init cache
    cache_.reset(new ChunkCache(config_.max_cache_size));

    // create volumes list
    for(auto path: v_iter.volume_names) {
        PVolume vol;
        vol.reset(new Volume(path.c_str(), config_, logger_));
        vol->make_readonly();
        volumes_.push_back(vol);
    }

    select_active_page();

    prepopulate_cache(config_.max_cache_size);
}

void Storage::close() {
    auto status = active_volume_->cache_->close(active_page_);
    if (status != AKU_SUCCESS) {
        std::stringstream fmt;
        fmt << "Can't merge cached values back to disk, some data would be lost. Reason: " << aku_error_message(status);
        log_error(fmt.str().c_str());
        return;
    }
    active_volume_->flush();
    // Update metadata store
    std::vector<SeriesMatcher::SeriesNameT> names;
    matcher_->pull_new_names(&names);
    if (!names.empty()) {
        metadata_->insert_new_names(names);
    }
}

void Storage::select_active_page() {
    // volume with max overwrites_count and max index must be active
    int max_index = -1;
    int64_t max_overwrites = -1;
    for(int i = 0; i < (int)volumes_.size(); i++) {
        PageHeader* page = volumes_.at(i)->get_page();
        if (static_cast<int64_t>(page->get_open_count()) >= max_overwrites) {
            max_overwrites = static_cast<int64_t>(page->get_open_count());
            max_index = i;
        }
    }

    active_volume_index_ = max_index;
    active_volume_ = volumes_.at(max_index);
    active_page_ = active_volume_->get_page();
    active_volume_->make_writable();

    if (active_page_->get_close_count() == active_page_->get_open_count()) {
        // Application was interrupted during volume
        // switching procedure
        advance_volume_(active_volume_index_.load());
    }
}

void Storage::prepopulate_cache(int64_t max_cache_size) {
    // All entries between sync_index (included) and count must
    // be cached.
    if (active_page_->restore()) {
        active_volume_->flush();
    }

    // Read data from sqlite to series matcher
    uint64_t nextid = 1 + metadata_->get_prev_largest_id();
    matcher_ = std::make_shared<SeriesMatcher>(nextid + 1);
    aku_Status status = metadata_->load_matcher_data(*matcher_);
    if (status != AKU_SUCCESS) {
        AKU_PANIC("Can't read series names from sqlite");
    }
}

aku_Status Storage::get_open_error() const {
    return open_error_code_;
}

void Storage::advance_volume_(int local_rev) {
    if (local_rev == active_volume_index_.load()) {
        log_message("advance volume, current:");
        log_message("....page ID", active_volume_->page_->get_page_id());
        log_message("....close count", active_volume_->page_->get_close_count());
        log_message("....open count", active_volume_->page_->get_open_count());

        auto old_page_id = active_page_->get_page_id();
        AKU_UNUSED(old_page_id);

        auto prev_volume = active_volume_;
        active_volume_->close();
        active_volume_->make_readonly();

        // select next page in round robin order
        active_volume_index_++;
        auto next_volume_index = active_volume_index_ % volumes_.size();
        auto next_volume = volumes_[next_volume_index];
        // swap next and prev volumes caches
        volumes_[next_volume_index] = next_volume->safe_realloc();

        active_volume_ = volumes_[next_volume_index];
        std::swap(active_volume_->cache_, prev_volume->cache_);

        active_volume_->open();
        active_volume_->make_writable();
        active_page_ = active_volume_->page_;

        auto new_page_id = active_page_->get_page_id();
        AKU_UNUSED(new_page_id);
        assert(new_page_id != old_page_id);

        log_message("next volume opened");
        log_message("....page ID", active_volume_->page_->get_page_id());
        log_message("....close count", active_volume_->page_->get_close_count());
        log_message("....open count", active_volume_->page_->get_open_count());
    }
    // Or other thread already done all the switching
    // just redo all the things
}

void Storage::log_message(const char* message) const {
    (*logger_)(AKU_LOG_INFO, message);
}

void Storage::log_error(const char* message) const {
    (*logger_)(AKU_LOG_ERROR, message);
}

void Storage::log_message(const char* message, uint64_t value) const {
    using namespace std;
    stringstream fmt;
    fmt << message << ", " << value;
    (*logger_)(AKU_LOG_INFO, fmt.str().c_str());
}

// Reading

struct SearchError : std::runtime_error
{
    aku_Status error_code;
    SearchError(const char* msg, aku_Status err) : std::runtime_error(msg), error_code(err) {}
};

struct TerminalNode : QP::Node {

    Caller &caller;
    InternalCursor* cursor;

    TerminalNode(Caller& ca, InternalCursor* cur)
        : caller(ca)
        , cursor(cur)
    {
    }

    // Node interface

    void complete() {
        cursor->complete(caller);
    }

    bool put(const aku_Sample& sample) {
        if (sample.payload.type != aku_PData::MARGIN) {
            return cursor->put(caller, sample);
        }
        return true;
    }

    void set_error(aku_Status status) {
        cursor->set_error(caller, status);
        throw SearchError("search error detected", status);
    }

    int get_requirements() const {
        return TERMINAL;
    }
};


void Storage::search(Caller &caller, InternalCursor* cur, const char* query) const {
    using namespace std;
    using namespace QP;

    try {
        // Parse query
        auto terminal_node = std::make_shared<TerminalNode>(caller, cur);
        std::shared_ptr<IQueryProcessor> query_processor;
        try {
            query_processor = Builder::build_query_processor(query, terminal_node, *matcher_, logger_);
        } catch (const QueryParserError& qpe) {
            log_error(qpe.what());
            cur->set_error(caller, AKU_EQUERY_PARSING_ERROR);
            return;
        }

        // Override series matcher
        auto matcher = query_processor->matcher();
        set_thread_local_matcher(matcher);

        if (query_processor->start()) {

            if (!query_processor->range().is_backward()) {
                uint32_t starting_ix = active_volume_->get_page()->get_page_id() + 1;  // Start from oldest volume
                for (uint32_t ix = starting_ix; ix < (starting_ix + volumes_.size()); ix++) {
                    // Search volume
                    uint32_t index = ix % volumes_.size();
                    PVolume volume = volumes_.at(index);
                    volume->get_page()->search(query_processor, cache_);
                    if (query_processor->range().type == QP::QueryRange::INSTANT) {
                        // Search cache
                        int seq_id;
                        aku_Timestamp window;
                        tie(window, seq_id) = volume->cache_->get_window();
                        volume->cache_->search(query_processor, seq_id);
                        // TODO: obtain `seq_id` before calling PageHeader::serach, if `seq_id` is outdated -
                        // obtain new `seq_id` and re-run PageHeader::search with adjusted query parameters (
                        // this is a tricky part because we should modify query_processor state a bit).
                    } else {
                        // Instead of searching cache we are using continuous querying feature here.
                        // We can read cache data only if we're interested in instant picture, for example if
                        // we're loocking for latest value.                     }

                        // NOTE: no operation required
                    }
                }
            } else if (query_processor->range().is_backward()) {
                uint32_t starting_ix = active_volume_->get_page()->get_page_id();  // Start from newest volume
                // TODO: handle case when `query_processor->range().type == QP::QueryRange::CONTINUOUS`
                // in this case we should wait until data with timestamp that matches `range.from` timestamp
                // will be written to disk and then start the query. Until this behavior become implemented
                // this set of parameter should trigger an error (AKU_ENOT_IMPLEMENTED).
                if (query_processor->range().type == QP::QueryRange::CONTINUOUS) {
                    SearchError error{"Continuous queries doesn't work in backward direction", AKU_ENOT_IMPLEMENTED};
                    BOOST_THROW_EXCEPTION(error);
                }
                for (int64_t ix = (starting_ix + volumes_.size()); ix > starting_ix; ix--) {
                    uint32_t index = static_cast<uint32_t>(ix % volumes_.size());
                    PVolume volume = volumes_.at(index);
                    // Search cache
                    int seq_id;
                    aku_Timestamp window;
                    tie(window, seq_id) = volume->cache_->get_window();
                    volume->cache_->search(query_processor, seq_id);
                    // Search volume
                    volume->get_page()->search(query_processor, cache_);
                }
            } else {
                AKU_PANIC("data corruption in query processor");
            }
            query_processor->stop();
        }
    }
    catch (const SearchError& err) {
        log_error(err.what());
    }
}


void Storage::get_stats(aku_StorageStats* rcv_stats) {
    for (PVolume const& vol: volumes_) {
        vol->page_->get_stats(rcv_stats);
    }
}

// Writing

aku_Status Storage::_write_impl(TimeSeriesValue ts_value, aku_MemRange data) {
    int local_rev = active_volume_index_.load();
    aku_Status status = AKU_SUCCESS;
    int merge_lock = 0;
    std::tie(status, merge_lock) = active_volume_->cache_->add(ts_value);
    switch (status) {
        case AKU_SUCCESS: {
            if (merge_lock % 2 == 1) {
                // Slow path //

                // Update metadata store
                std::vector<SeriesMatcher::SeriesNameT> names;
                matcher_->pull_new_names(&names);
                if (!names.empty()) {
                    metadata_->insert_new_names(names);
                }

                // Move data from cache to disk
                status = active_volume_->cache_->merge_and_compress(active_volume_->get_page());
                switch (status) {
                case AKU_SUCCESS:
                    switch(config_.durability) {
                    case AKU_MAX_DURABILITY:
                        // Max durability
                        active_volume_->flush();
                        break;
                    case AKU_DURABILITY_SPEED_TRADEOFF:
                        // Compromice some durability for speed
                        if ((merge_lock % 8) == 1) {
                            active_volume_->flush();
                        }
                        break;
                    case AKU_MAX_WRITE_SPEED:
                        break;
                    };
                    break;
                case AKU_EOVERFLOW:
                    // Page overflow
                    advance_volume_(local_rev);
                    status = AKU_SUCCESS;  // Value is stored by cache so it wouldn't be lost
                    break;
                default:
                    log_error(aku_error_message(status));
                    AKU_PANIC("Fatal error in write path");
                    break;
                };
            }
        }
        default:
            break;
    }
    return status;
}

//! write binary data
aku_Status Storage::write_double(aku_ParamId param, aku_Timestamp ts, double value) {
    aku_MemRange m = {};
    TimeSeriesValue ts_value(ts, param, value);
    return _write_impl(ts_value, m);
}

aku_Status Storage::series_to_param_id(const char* begin, const char* end, uint64_t *value) {
    char buffer[AKU_LIMITS_MAX_SNAME];
    const char* keystr_begin = nullptr;
    const char* keystr_end = nullptr;
    auto status = SeriesParser::to_normal_form(begin, end,
                                               buffer, buffer+AKU_LIMITS_MAX_SNAME,
                                               &keystr_begin, &keystr_end);
    if (status == AKU_SUCCESS) {
        auto id = matcher_->match(buffer, keystr_end);
        if (id == 0) {
            *value = matcher_->add(buffer, keystr_end);
        } else {
            *value = id;
        }
    }
    return status;
}

void Storage::set_thread_local_matcher(SeriesMatcher *matcher) const {
    local_matcher_.reset(matcher);
}

int Storage::param_id_to_series(aku_ParamId id, char* buffer, size_t buffer_size) const {
    SeriesMatcher const* m;
    if (local_matcher_.get() != nullptr) {
        // Series matcher overriden by query (group by tag stmtm)
        m = local_matcher_.get();
    } else {
        // Use global matcher
        m = matcher_.get();
    }
    auto str = m->id2str(id);
    if (str.first == nullptr) {
        return 0;
    }
    if (str.second >= (int)buffer_size) {
        // buffer is too small
        return -1*(str.second + 1);
    }
    memcpy(buffer, str.first, str.second);
    buffer[str.second] = '\0';
    return str.second + 1;
}

void Storage::debug_print() const {
    for (auto vol: volumes_) {
        std::cout << "Volume id: " << vol->get_page()->get_page_id() << std::endl;
        std::cout << "    num chunks: " << vol->get_page()->get_entries_count() << std::endl;
        std::cout << "    free space: " << vol->get_page()->get_free_space() << std::endl;
        std::cout << "    open count: " << vol->get_page()->get_open_count() << std::endl;
        std::cout << "   close count: " << vol->get_page()->get_close_count() << std::endl;
        std::cout << "     num pages: " << vol->get_page()->get_numpages() << std::endl;
    }
}


// Standalone functions //


/** This function creates file with specified size
  */
static apr_status_t create_file(const char* file_name, uint64_t size, aku_logger_cb_t logger) {
    using namespace std;
    apr_status_t status;
    int success_count = 0;
    apr_pool_t* mem_pool = NULL;
    apr_file_t* file = NULL;

    status = apr_pool_create(&mem_pool, NULL);
    if (status == APR_SUCCESS) {
        success_count++;

        // Create new file
        status = apr_file_open(&file, file_name, APR_CREATE|APR_WRITE, APR_OS_DEFAULT, mem_pool);
        if (status == APR_SUCCESS) {
            success_count++;

            // Truncate file
            status = apr_file_trunc(file, size);
            if (status == APR_SUCCESS)
                success_count++;
        }
    }

    if (status != APR_SUCCESS) {
        char error_message[0x100];
        apr_strerror(status, error_message, 0x100);
        stringstream err;
        err << "Can't create file, error " << error_message << " on step " << success_count;
        (*logger)(AKU_LOG_ERROR, err.str().c_str());
    }

    switch(success_count) {
    case 3:
    case 2:
        status = apr_file_close(file);
    case 1:
        apr_pool_destroy(mem_pool);
    case 0:
        // even apr pool is not created
        break;
    }
    return status;
}


/** This function creates one of the page files with specified
  * name and index.
  */
static apr_status_t create_page_file(const char* file_name, uint32_t page_index, uint32_t npages, aku_logger_cb_t logger, uint64_t page_size) {
    using namespace std;
    apr_status_t status;
    int64_t size = page_size == 0 ? AKU_MAX_PAGE_SIZE : page_size;

    status = create_file(file_name, size, logger);
    if (status != APR_SUCCESS) {
        stringstream err;
        err << "Can't create page file " << file_name;
        (*logger)(AKU_LOG_ERROR, err.str().c_str());
        return status;
    }

    MemoryMappedFile mfile(file_name, false, logger);
    if (mfile.is_bad())
        return mfile.status_code();

    // Create index page
    auto index_ptr = mfile.get_pointer();
    auto index_page = new (index_ptr) PageHeader(0, size, page_index, npages);

    // Activate the first page
    if (page_index == 0) {
        index_page->reuse();
    }
    return status;
}

/** Create page files, return list of statuses.
  */
static std::vector<apr_status_t> create_page_files(std::vector<std::string> const& targets, aku_logger_cb_t logger, uint64_t page_size) {
    std::vector<apr_status_t> results(targets.size(), APR_SUCCESS);
    for (size_t ix = 0; ix < targets.size(); ix++) {
        apr_status_t res = create_page_file(targets[ix].c_str(), (uint32_t)ix, (uint32_t)targets.size(), logger, page_size);
        results[ix] = res;
    }
    return results;
}

static std::vector<apr_status_t> delete_files(const std::vector<std::string>& targets,
                                              const std::vector<apr_status_t>& statuses,
                                              aku_logger_cb_t logger)
{
    using namespace std;
    if (targets.size() != statuses.size()) {
        AKU_PANIC("sizes of targets and statuses doesn't match");
    }
    apr_pool_t* mem_pool = NULL;
    int op_count = 0;
    apr_status_t status = apr_pool_create(&mem_pool, NULL);
    std::vector<apr_status_t> results;
    if (status == APR_SUCCESS) {
        op_count++;
        for(auto ix = 0u; ix < targets.size(); ix++) {
            const std::string& target = targets[ix];
            if (statuses[ix] == APR_SUCCESS) {
                stringstream fmt;
                fmt << "Removing " << target;
                (*logger)(AKU_LOG_ERROR, fmt.str().c_str());
                status = apr_file_remove(target.c_str(), mem_pool);
                results.push_back(status);
                if (status != APR_SUCCESS) {
                    char error_message[1024];
                    apr_strerror(status, error_message, 1024);
                    stringstream err;
                    err << "Error [" << error_message << "] while deleting a file " << target;
                    (*logger)(AKU_LOG_ERROR, err.str().c_str());
                }
            }
            else {
                stringstream fmt;
                fmt << "Target " << target << " doesn't need to be removed";
                (*logger)(AKU_LOG_ERROR, fmt.str().c_str());
            }
        }
    }
    if (op_count) {
        apr_pool_destroy(mem_pool);
    }
    return results;
}

/** This function creates metadata file - root of the storage system.
  * This page contains creation date and time, number of pages,
  * all the page file names and they order.
  * @return APR_EINIT on DB error.
  */
static apr_status_t create_metadata_page( const char* file_name
                                        , std::vector<std::string> const& page_file_names
                                        , aku_logger_cb_t logger)
{
    using namespace std;
    try {
        auto storage = std::make_shared<MetadataStorage>(file_name, logger);

        auto now = apr_time_now();
        char date_time[0x100];
        apr_rfc822_date(date_time, now);

        storage->init_config(date_time);

        std::vector<MetadataStorage::VolumeDesc> desc;
        int ix = 0;
        for(auto str: page_file_names) {
            desc.push_back(std::make_pair(ix++, str));
        }
        storage->init_volumes(desc);

    } catch (std::exception const& err) {
        std::stringstream fmt;
        fmt << "Can't create metadata file " << file_name << ", the error is: " << err.what();
        (*logger)(AKU_LOG_ERROR, fmt.str().c_str());
        return APR_EGENERAL;
    }
    return APR_SUCCESS;
}


apr_status_t Storage::new_storage(const char     *file_name,
                                  const char     *metadata_path,
                                  const char     *volumes_path,
                                  int             num_pages,
                                  aku_logger_cb_t logger,
                                  uint64_t        page_size)
{
    if (page_size > AKU_MAX_PAGE_SIZE) {
        return APR_EINVAL;
    }
    apr_pool_t* mempool;
    apr_status_t status = apr_pool_create(&mempool, NULL);
    if (status != APR_SUCCESS) {
        return status;
    }

    // get absolute volumes and metadata path
    boost::filesystem::path volpath(volumes_path);
    boost::filesystem::path metpath(metadata_path);
    volpath = boost::filesystem::absolute(volpath);
    metpath = boost::filesystem::absolute(metpath);

    // calculate list of page-file names
    std::vector<std::string> page_names;
    for (int ix = 0; ix < num_pages; ix++) {
        std::stringstream stream;
        stream << file_name << "_" << ix << ".volume";
        char* path = nullptr;
        std::string volume_file_name = stream.str();
        status = apr_filepath_merge(&path, volpath.c_str(), volume_file_name.c_str(), APR_FILEPATH_NATIVE, mempool);
        if (status != APR_SUCCESS) {
            auto error_message = apr_error_message(status);
            std::stringstream err;
            err << "Invalid volumes path: " << error_message;
            (*logger)(AKU_LOG_ERROR, err.str().c_str());
            apr_pool_destroy(mempool);
            AKU_APR_PANIC(status, error_message.c_str());
        }
        page_names.push_back(path);
    }

    apr_pool_clear(mempool);

    status = apr_dir_make(metpath.c_str(), APR_OS_DEFAULT, mempool);
    if (status == APR_EEXIST) {
        (*logger)(AKU_LOG_INFO, "Metadata dir already exists");
    }
    status = apr_dir_make(volpath.c_str(), APR_OS_DEFAULT, mempool);
    if (status == APR_EEXIST) {
        (*logger)(AKU_LOG_INFO, "Volumes dir already exists");
    }

    std::vector<apr_status_t> page_creation_statuses = create_page_files(page_names, logger, page_size);
    for(auto creation_status: page_creation_statuses) {
        if (creation_status != APR_SUCCESS) {
            (*logger)(AKU_LOG_ERROR, "Not all pages successfullly created. Cleaning up.");
            apr_pool_destroy(mempool);
            delete_files(page_names, page_creation_statuses, logger);
            return creation_status;
        }
    }

    std::stringstream stream;
    stream << file_name << ".akumuli";
    char* path = nullptr;
    std::string metadata_file_name = stream.str();
    status = apr_filepath_merge(&path, metpath.c_str(), metadata_file_name.c_str(), APR_FILEPATH_NATIVE, mempool);
    if (status != APR_SUCCESS) {
        auto error_message = apr_error_message(status);
        std::stringstream err;
        err << "Invalid metadata path: %s" << error_message;
        (*logger)(AKU_LOG_ERROR, err.str().c_str());
        apr_pool_destroy(mempool);
        AKU_APR_PANIC(status, error_message.c_str());
    }
    status = create_metadata_page(path, page_names, logger);
    apr_pool_destroy(mempool);
    return status;
}


apr_status_t Storage::remove_storage(const char* file_name, aku_logger_cb_t logger) {
    std::shared_ptr<MetadataStorage> db;
    try {
        db = std::make_shared<MetadataStorage>(file_name, logger);
    } catch(std::runtime_error const& err) {
        (*logger)(AKU_LOG_ERROR, err.what());
        return APR_EBADPATH;
    }

    VolumeIterator v_iter(db, logger);

    if (v_iter.is_bad()) {
        return v_iter.error_code;
    }

    apr_pool_t* mempool;
    apr_status_t status = apr_pool_create(&mempool, NULL);

    if (status != APR_SUCCESS) {
        (*logger)(AKU_LOG_ERROR, "can't create memory pool");
        return status;
    }

    // create volumes list
    for(auto path: v_iter.volume_names) {
        status = apr_file_remove(path.c_str(), mempool);
        if (status != APR_SUCCESS) {
            std::stringstream fmt;
            fmt << "can't remove file " << path;
            (*logger)(AKU_LOG_ERROR, fmt.str().c_str());
        }
    }

    status = apr_file_remove(file_name, mempool);
    apr_pool_destroy(mempool);
    return status;
}

}
