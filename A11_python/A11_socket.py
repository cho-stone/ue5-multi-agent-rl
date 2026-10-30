#CHO_SEOKJIN

import socket
import json

class UESocketServer:
    # Create a TCP socket
    def __init__(self, host='127.0.0.1', port=9001, listen_backlog=1):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((host, port))
        self.sock.listen(listen_backlog)
        print(f"python socket server ({host}:{port}) waiting...")
        self.conn, self.addr = self.sock.accept()
        print(f"Connect UE: {self.addr}")
        self.buffer = ""

    """
    Send an action command string to the connected Unreal Engine client.
    The string is encoded as bytes before sending.
    """
    def send_action(self, action_str):
        self.conn.sendall(action_str.encode())

    """
    Receive a state message(JSON) from the Unreal Engine client.
    The function reads data until a newline character is found,
    splits off one line, and attempts to parse it as JSON.
    If parsing fails, returns None.
    """
    def receive_state(self):
        while "\n" not in self.buffer:
            chunk = self.conn.recv(4096).decode()
            if not chunk:
                raise RuntimeError("Disconnect UE")
            self.buffer += chunk

        line, self.buffer = self.buffer.split("\n", 1)
        if not line.strip():
            return self.receive_state()
        try:
            state = json.loads(line)
        except Exception as e:
            print('json parsing error:', e, 'data=', line)
            state = None
        return state
    
    """
    Close the socket connection and release resources.
    """
    def close(self):
        self.conn.close()
        self.sock.close()
