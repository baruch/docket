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

## License

MIT License, see LICENSE file for full text.

## Author

Baruch Even <baruch@ev-en.org>
