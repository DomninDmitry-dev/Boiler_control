#!/bin/sh

while true; do
clear;
echo "Time:"
date
printf "status" | nc -w 1 192.168.1.200 50
sleep 5;
done
