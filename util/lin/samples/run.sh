#!/bin/bash
make
cp aws ../../../checker-lin
cd ../../../checker-lin
make -f Makefile.checker
cd -