#!/usr/bin/python

import os
import sys
import time

NUM_EVENT_QUEUES = 10
NUM_FILES = 10

mountpoint = sys.argv[1]
if not os.path.exists( mountpoint ):
    print >> sys.stderr, "Usage: %s MOUNTPOINT" % sys.argv[0]
    sys.exit(1)

for i in xrange(0, NUM_EVENT_QUEUES):

    print "event queue: %s/test-%s" % (mountpoint, i)
    os.mkdir( "%s/test-%s" % (mountpoint, i) )

# link the first 
with open( "%s/test-0/input" % mountpoint, "w+" ) as f:
    f.write("event text")

for i in xrange(1, NUM_EVENT_QUEUES):
    os.link( "%s/test-0/input" % mountpoint, "%s/test-%s/link" % (mountpoint, i))

print "Waiting for SIGINT"
while True:
    time.sleep(100)

