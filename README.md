# docket

Docket is intended as a log collector for a cluster system, it works as a
daemon on each server and a client that can connect to one or many daemons and
generate a single tarball of all needed log and state files from the system to
remotely debug it or to just keep tab of a point-in-time state of the cluster.

The client gives to the daemons a list of items to collect, be they files,
outputs of commands to run or entire trees or globs of files.

Examples of files to collect include:

* /var/log/messages
* /proc/meminfo
* /sbin/route -n

## Command Structure

A command is made up of a pipe '|' seperated line, for example:

    FILE|proc|/proc/meminfo

The first part is the command type, the next is the directory to put it into
for grouping purposes and the third and onwards is the command and it's
arguments.

Available commands:
* PREFIX -- Set global directory prefix for when collecting from multiple server
* FILE -- Collect a single file
* GLOB -- Collect a group of files based on a glob
* FIND -- Collect a group of files based on the find command terms
* EXEC -- Collect the output of a command, both stdout and stderr

## License

MIT License, see LICENSE file for full text.

## Author

Baruch Even <baruch@ev-en.org>
