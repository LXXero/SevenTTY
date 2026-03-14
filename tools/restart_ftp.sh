#!/bin/bash
# Kill any existing FTP test server and restart on port 21
sudo pkill -f ftp_test_server.py 2>/dev/null
sleep 0.5
cd "$(dirname "$0")"
sudo python3 ftp_test_server.py 21 &
echo "FTP test server started on port 21 (pid $!)"
