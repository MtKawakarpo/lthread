#!/usr/bin/env bash

sudo ./build/l3fwd-thread -c 0xff -n 4 -w 81:00.0 -w 81:00.1 -- -p 0x03 --rx="(0,1,0,0)(0,2,0,1)(0,3,0,2)" --tx="(1,0)(1,1)(1,2)"