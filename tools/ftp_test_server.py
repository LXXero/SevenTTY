#!/usr/bin/env python3
"""Minimal FTP test server with working PASV data connections.
Usage: python3 ftp_test_server.py [port]
Connect from Mac: ftp get anonymous@10.0.2.2:<port>:/test.txt
(10.0.2.2 is the QEMU SLIRP host gateway)
"""
import socket, sys, time, threading

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 2121

# fake file content served for any RETR
FILE_DATA = b"Hello from the FTP test server!\r\nThis is test file data.\r\n"

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", PORT))
srv.listen(1)
print(f"FTP test server on port {PORT}")
print(f"From Mac QEMU, connect to: ftp get anonymous@10.0.2.2:{PORT}:/test.txt")

def send_resp(conn, resp):
    conn.sendall(resp.encode())
    print(f"SENT: {resp.strip()}")

while True:
    conn, addr = srv.accept()
    print(f"\n=== Connection from {addr} ===")

    send_resp(conn, "220 Test FTP server ready.\r\n")

    pasv_sock = None  # listening socket for PASV data connection
    data_conn = None   # accepted data connection

    try:
        while True:
            data = conn.recv(4096)
            if not data:
                print("Client disconnected")
                break

            lines = data.decode('ascii', errors='replace').strip().split('\r\n')
            for line in lines:
                print(f"RECV: {line}")

                parts = line.split()
                cmd = parts[0].upper() if parts else ""

                if cmd == "USER":
                    # simulate ftp.gnu.org's multiline 230 response
                    send_resp(conn, "230-NOTICE (Updated October 15 2021):\r\n"
                        "230-\r\n"
                        "230-If you maintain scripts used to access ftp.gnu.org over FTP,\r\n"
                        "230-we strongly encourage you to change them to use HTTPS instead.\r\n"
                        "230-\r\n"
                        "230-Eventually we hope to shut down FTP protocol access, but plan\r\n"
                        "230-to note here and other places for several months ahead\r\n"
                        "230-of time.\r\n"
                        "230-\r\n"
                        "230-----\r\n"
                        "230-\r\n"
                        "230-\r\n"
                        "230-Due to U.S. Export Regulations, all cryptographic software on this\r\n"
                        "230-site is subject to the following legal notice:\r\n"
                        "230-\r\n"
                        "230-    This site includes publicly available encryption source code\r\n"
                        "230-    which, together with object code resulting from the compiling of\r\n"
                        "230-    publicly available source code, may be exported from the United\r\n"
                        "230-    States under License Exception TSU pursuant to 15 C.F.R. Section\r\n"
                        "230-    740.13(e).\r\n"
                        "230-\r\n"
                        "230-This legal notice applies to cryptographic software only. Please see\r\n"
                        "230-the Bureau of Industry and Security (www.bxa.doc.gov) for more\r\n"
                        "230-information about current U.S. regulations.\r\n"
                        "230 Login successful.\r\n")
                elif cmd == "PASS":
                    send_resp(conn, "230 Login successful.\r\n")
                elif cmd == "SYST":
                    send_resp(conn, "215 UNIX Type: L8\r\n")
                elif cmd == "TYPE":
                    send_resp(conn, "200 Type set.\r\n")
                elif cmd == "PASV":
                    # open a real data listener on an ephemeral port
                    if pasv_sock:
                        pasv_sock.close()
                    pasv_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    pasv_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                    pasv_sock.bind(("0.0.0.0", 0))
                    pasv_sock.listen(1)
                    pasv_sock.settimeout(30)
                    dp = pasv_sock.getsockname()[1]
                    p1, p2 = dp // 256, dp % 256
                    send_resp(conn, f"227 Entering Passive Mode (0,0,0,0,{p1},{p2})\r\n")
                    print(f"  (data port: {dp})")
                elif cmd == "SIZE":
                    send_resp(conn, f"213 {len(FILE_DATA)}\r\n")
                elif cmd == "RETR":
                    send_resp(conn, "150 Opening BINARY mode data connection.\r\n")
                    # accept data connection and send file
                    try:
                        if pasv_sock:
                            data_conn, daddr = pasv_sock.accept()
                            print(f"  Data connection from {daddr}")
                            data_conn.sendall(FILE_DATA)
                            data_conn.close()
                            data_conn = None
                            pasv_sock.close()
                            pasv_sock = None
                        send_resp(conn, "226 Transfer complete.\r\n")
                    except Exception as e:
                        print(f"  Data transfer error: {e}")
                        send_resp(conn, "426 Connection closed; transfer aborted.\r\n")
                elif cmd == "STOR":
                    send_resp(conn, "150 Ok to send data.\r\n")
                    try:
                        if pasv_sock:
                            data_conn, daddr = pasv_sock.accept()
                            print(f"  Data connection from {daddr}")
                            received = b""
                            while True:
                                chunk = data_conn.recv(4096)
                                if not chunk:
                                    break
                                received += chunk
                            data_conn.close()
                            data_conn = None
                            pasv_sock.close()
                            pasv_sock = None
                            print(f"  Received {len(received)} bytes")
                        send_resp(conn, "226 Transfer complete.\r\n")
                    except Exception as e:
                        print(f"  Data transfer error: {e}")
                        send_resp(conn, "426 Connection closed; transfer aborted.\r\n")
                elif cmd == "LIST":
                    send_resp(conn, "150 Here comes the directory listing.\r\n")
                    try:
                        if pasv_sock:
                            data_conn, daddr = pasv_sock.accept()
                            listing = f"-rw-r--r--   1 ftp  ftp  {len(FILE_DATA)} Jan  1 00:00 test.txt\r\n"
                            data_conn.sendall(listing.encode())
                            data_conn.close()
                            data_conn = None
                            pasv_sock.close()
                            pasv_sock = None
                        send_resp(conn, "226 Directory send OK.\r\n")
                    except Exception as e:
                        print(f"  Data transfer error: {e}")
                        send_resp(conn, "426 Connection closed; transfer aborted.\r\n")
                elif cmd == "QUIT":
                    send_resp(conn, "221 Goodbye.\r\n")
                    raise ConnectionResetError
                elif cmd == "PWD":
                    send_resp(conn, '257 "/" is current directory.\r\n')
                elif cmd == "CWD":
                    send_resp(conn, "250 Directory changed.\r\n")
                elif cmd == "MKD":
                    send_resp(conn, "257 Directory created.\r\n")
                else:
                    send_resp(conn, f"502 Command not implemented.\r\n")
    except (BrokenPipeError, ConnectionResetError, OSError) as e:
        print(f"Connection ended: {e}")
    if pasv_sock:
        pasv_sock.close()
    if data_conn:
        data_conn.close()
    conn.close()
