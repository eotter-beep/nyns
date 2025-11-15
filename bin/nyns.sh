#!/bin/bash
interpret_command() {
    # Split the command into words
    read -r command_type arg1 arg2 <<< "$1"

    if [ "$command_type" = "echo" ]; then
        # PRINT command: simply echo the rest of the arguments
        echo "${arg1} ${arg2}"
    elif [ "$command_type" = "+" ]; then
        # ADD command: perform an arithmetic addition
        sum=$((arg1 + arg2))
        echo "$sum"
    elif [ "$command_type" = "-" ]; then
        # ADD command: perform an arithmetic addition
        sum=$((arg1 - arg2))
        echo "$sum"
    elif [ "$command_type" = "rem" ]; then
        rm -rf ${arg1}
    elif [ "$command_type" = "rem -f" ]; then
        rm -f ${arg1}
    elif [ "$command_type" = "rem" ]; then
        rm -rf ${arg1}
    elif [ "$command_type" = "moveto" ]; then
        cd -- "${arg1}"
    elif [ "$command_type" = "help" ]; then
        echo "echo: Displays text on-screen"
        echo "+: Addition"
        echo "-: Removal of number"
        echo "rem: Delete a path (irreversible)"
        echo "rem arguments: -f: Forced deletion"
        echo "moveto: CD into a directory"
        echo "help: Get command help"
        echo "ip: Get IP address"
        echo "create: Create a file"
        echo "adm: Run a command as admin"
    elif [ "$command_type" = "ip" ]; then
        ip addr
    elif [ "$command_type" = "create" ]; then
        touch ${arg1}
    elif [ "$command_type" = "adm" ]; then
        sudo ${arg1}
    elif [ "$command_type" = "partition" ]; then
        fdisk ${arg1}
    else
        echo "Error: Unknown command '$command_type'"
    fi
}

# Main loop to read and interpret lines from an input file
while IFS= read -r line; do
    # Skip empty lines and comments (lines starting with #)
    if [ -z "$line" ] || [[ "$line" =~ ^#.* ]]; then
        continue
    fi
    interpret_command "$line"
done < "$1"
