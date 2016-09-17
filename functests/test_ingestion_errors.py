from __future__ import print_function
import os
import sys
import socket
import datetime
import time
import akumulid_test_tools as att
import json
from socket import *
try:
    import urllib2 as urllib
except ImportError:
    import urllib
import traceback

EP = ("127.0.0.1", 8282)

MSG = [
    '\n',			# issue #94
    '+metric\r\n:123\r\n+5.0',  # invalid series name - no tags specified, issue #96
]

def send_malicious_message(ix):
    s = socket(AF_INET, SOCK_STREAM)
    s.settimeout(1.0)
    s.connect(EP)
    s.send(MSG[ix])
    res = s.recv(1024)  # this will hang on success but will
                        # return error message in our case
                        # because we're sending bad data
    s.close()
    return res

def main(path):

    if not os.path.exists(path):
        print("Path {0} doesn't exists".format(path))
        sys.exit(1)

    akumulid = att.Akumulid(path)
    # delete database
    akumulid.delete_database()
    # create empty database
    akumulid.create_database()
    # start ./akumulid server
    print("Starting server...")
    akumulid.serve()
    time.sleep(5)
    try:
        for ix in range(0, len(MSG)):
            result = send_malicious_message(ix)
            if not result.startswith('-ERR'):
                print("Error at {0}".format(ix))
                print("Message:\n{0}".format(MSG[ix]))
                print("Response:\n{0}".format(result))
                raise ValueError("Bad response")
    except:
        traceback.print_exc()
        sys.exit(1)
    finally:
        print("Stopping server...")
        akumulid.stop()
        time.sleep(5)

if __name__ == '__main__':
    if len(sys.argv) != 2:
        print("Not enough arguments")
        sys.exit(1)
    main(sys.argv[1])
else:
    raise ImportError("This module shouldn't be imported")
