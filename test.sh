#!/bin/bash

echo "Test log" > /tmp/test.log

echo -ne 'PREFIX|prefix\nFILE|dir1|/proc/meminfo\nFILE|dir2|/tmp/test.log\nEOF\n' | nc 127.0.0.1 7000
