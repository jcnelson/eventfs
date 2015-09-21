#!/usr/bin/python

import os
import sys
import time

NUM_EVENT_QUEUES = 1
NUM_FILES = 10

mountpoint = sys.argv[1]
if not os.path.exists( mountpoint ):
    print >> sys.stderr, "Usage: %s MOUNTPOINT" % sys.argv[0]
    sys.exit(1)

for i in xrange(0, NUM_EVENT_QUEUES):

    print "event queue: tmp/test-%s" % i
    os.mkdir( "tmp/test-%s" % i )

    for j in xrange(0, NUM_FILES):
        path = "tmp/test-%s/%s" % (i,j)
        print "event message: %s" % path 

        with open(path, "w+") as f:
            f.write("%s\n" % j)
        
    

print "Waiting for SIGINT"
while True:
    time.sleep(100)

