"""
Simple HTTP server for testing the proxy
Run with: python test_server.py
"""
from http.server import HTTPServer, BaseHTTPRequestHandler
import time
import json

class TestHandler(BaseHTTPRequestHandler):
    def _set_headers(self, content_type="text/html"):
        self.send_response(200)
        self.send_header("Content-type", content_type)
        self.send_header("Server", "TestServer/1.0")
        self.end_headers()

    def do_GET(self):
        # Log request info
        print(f"Received request for: {self.path}")
        print(f"Headers: {self.headers}")
        
        if self.path == "/":
            self._set_headers()
            self.wfile.write(b"""
            <html>
            <head><title>Test Server</title></head>
            <body>
                <h1>Test Server</h1>
                <p>This is a test server for the proxy server.</p>
                <ul>
                    <li><a href="/text">Plain Text Response</a></li>
                    <li><a href="/json">JSON Response</a></li>
                    <li><a href="/large">Large Response</a></li>
                    <li><a href="/delayed">Delayed Response (3s)</a></li>
                </ul>
            </body>
            </html>
            """)
        elif self.path == "/text":
            self._set_headers("text/plain")
            self.wfile.write(b"This is a plain text response")
        elif self.path == "/json":
            self._set_headers("application/json")
            data = {
                "message": "This is a JSON response",
                "timestamp": time.time(),
                "status": "success"
            }
            self.wfile.write(json.dumps(data).encode())
        elif self.path == "/large":
            self._set_headers()
            # Generate a ~100KB response
            large_content = "<h1>Large Response</h1>\n"
            for i in range(1000):
                large_content += f"<p>Line {i}: {'*' * 100}</p>\n"
            self.wfile.write(large_content.encode())
        elif self.path == "/delayed":
            time.sleep(3)  # Delay for 3 seconds
            self._set_headers()
            self.wfile.write(b"<h1>Delayed Response</h1><p>This response was delayed by 3 seconds.</p>")
        else:
            self.send_response(404)
            self.send_header("Content-type", "text/html")
            self.end_headers()
            self.wfile.write(b"<h1>404 Not Found</h1>")

    def do_POST(self):
        content_length = int(self.headers['Content-Length'])
        post_data = self.rfile.read(content_length)
        
        print(f"Received POST to: {self.path}")
        print(f"Headers: {self.headers}")
        print(f"Data: {post_data.decode()}")
        
        self._set_headers("application/json")
        response = {
            "status": "success",
            "message": "POST request received",
            "data_size": content_length
        }
        self.wfile.write(json.dumps(response).encode())

def run(server_class=HTTPServer, handler_class=TestHandler, port=8000):
    server_address = ('', port)
    httpd = server_class(server_address, handler_class)
    print(f"Starting test server on port {port}...")
    httpd.serve_forever()

if __name__ == "__main__":
    run()
