#!/bin/bash

# Check if the correct number of arguments is provided
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 <path> <string>"
    exit 1
fi

# Assign arguments to variables
path=$1
string=$2

# Check if the provided path is a file or directory
if [ -d "$path" ]; then
    echo "$path is a directory"
    file_count=$(find "$path" -type f | wc -l)
    line_count=$(grep -ro "$string" "$path" | wc -l)
    echo " The number of files are $file_count and the number of matching lines are $line_count"
elif [ -f "$path" ]; then
    echo "$path is a file"
    line_count=$(grep -o "$string" "$path" | wc -l)
    echo " The number of file is 1 and the number of matching lines are $line_count"

else
    #echo "Error: $path is neither a file nor a directory"
    exit 1
fi



