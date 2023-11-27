#!/bin/bash

# get name of the experiment from command line argument
# have default value of "default" if no argument is passed
if [ -z "$1" ]
then
    echo "No argument supplied"
    number=$(ls -l results/ | wc -l)
    name="default-$number"
else
    name=$1
fi

sudo ./remote-perf-https-multicore-x86 1 24

# move results to results folder
mkdir -p results
mv collection.csv results/collection-$name.csv