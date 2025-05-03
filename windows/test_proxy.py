"""
Test script for proxy server
Run with: python test_proxy.py
"""
import requests
import time
import sys
import threading

# Configuration
PROXY_HOST = "127.0.0.1"
PROXY_PORT = 8080  # Default proxy port
TEST_SERVER_PORT = 8000  # Default test server port

# Set up proxy address
proxy_url = f"http://{PROXY_HOST}:{PROXY_PORT}"
proxies = {
    "http": proxy_url,
    "https": proxy_url
}

def test_request(path, expected_status=200, description=""):
    """Make a request through the proxy and verify the response"""
    url = f"http://localhost:{TEST_SERVER_PORT}{path}"
    print(f"\nTesting: {description}")
    print(f"URL: {url}")
    
    try:
        start_time = time.time()
        response = requests.get(url, proxies=proxies, timeout=10)
        end_time = time.time()
        
        print(f"Status: {response.status_code}")
        print(f"Time: {end_time - start_time:.2f}s")
        print(f"Content length: {len(response.content)} bytes")
        
        if response.status_code == expected_status:
            print("✓ Test passed")
            return True
        else:
            print(f"✗ Test failed - Expected status {expected_status}, got {response.status_code}")
            return False
            
    except Exception as e:
        print(f"✗ Test failed - Exception: {e}")
        return False

def test_cache(path, description=""):
    """Test caching by making the same request twice and comparing times"""
    url = f"http://localhost:{TEST_SERVER_PORT}{path}"
    print(f"\nTesting cache: {description}")
    print(f"URL: {url}")
    
    try:
        # First request (should miss cache)
        start_time = time.time()
        response1 = requests.get(url, proxies=proxies, timeout=10)
        end_time = time.time()
        first_time = end_time - start_time
        print(f"First request time: {first_time:.2f}s (cache miss expected)")
        
        # Second request (should hit cache)
        start_time = time.time()
        response2 = requests.get(url, proxies=proxies, timeout=10)
        end_time = time.time()
        second_time = end_time - start_time
        print(f"Second request time: {second_time:.2f}s (cache hit expected)")
        
        # Verify responses match
        if response1.content == response2.content:
            print("✓ Responses match")
        else:
            print("✗ Responses don't match")
            return False
        
        # Check if second request was faster (indicating cache hit)
        if second_time < first_time:
            print("✓ Second request faster (likely cached)")
            return True
        else:
            print("? Second request not faster (cache may not be working)")
            return False
            
    except Exception as e:
        print(f"✗ Test failed - Exception: {e}")
        return False

def run_tests():
    """Run all tests"""
    success_count = 0
    total_tests = 6
    
    # Basic GET requests
    if test_request("/", description="Basic HTML page"):
        success_count += 1
    
    if test_request("/text", description="Plain text response"):
        success_count += 1
    
    if test_request("/json", description="JSON response"):
        success_count += 1
    
    if test_request("/nonexistent", 404, description="404 Not Found"):
        success_count += 1
    
    # Cache tests
    if test_cache("/delayed", description="Cached delayed response"):
        success_count += 1
    
    if test_cache("/large", description="Cached large response"):
        success_count += 1
    
    # Print summary
    print("\n--- Test Summary ---")
    print(f"Passed: {success_count}/{total_tests}")
    
    if success_count == total_tests:
        print("All tests passed! The proxy server is working correctly.")
    else:
        print(f"Some tests failed. Check the logs above for details.")

if __name__ == "__main__":
    print("=== Proxy Server Test ===")
    print(f"Using proxy at: {proxy_url}")
    print(f"Testing against server at: http://localhost:{TEST_SERVER_PORT}\n")
    
    if len(sys.argv) > 1:
        PROXY_PORT = int(sys.argv[1])
    
    run_tests()
