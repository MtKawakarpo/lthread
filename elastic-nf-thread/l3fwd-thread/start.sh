#!/usr/bin/env bash

sudo ./build/l3fwd-thread -c 0xff -n 4 -w 81:00.0 -w 81:00.1 -- -p 0x03