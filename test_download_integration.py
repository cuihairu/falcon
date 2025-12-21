#!/usr/bin/env python3
"""
Integration test script for Falcon Downloader
Tests HTTP download functionality
"""

import subprocess
import os
import sys
import time
import json
from pathlib import Path

def test_http_download():
    """Test basic HTTP download"""
    print("Testing HTTP download...")

    # Output file
    output_file = "test_download.json"

    # Run falcon-cli
    cmd = [f"./bin/falcon-cli",
           "https://httpbin.org/json",
           "-o", output_file]

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)

        if result.returncode != 0:
            print(f"❌ Download failed with exit code {result.returncode}")
            print(f"STDOUT: {result.stdout}")
            print(f"STDERR: {result.stderr}")
            return False

        # Check if file exists
        if os.path.exists(output_file):
            file_size = os.path.getsize(output_file)
            print(f"✅ Download successful! File size: {file_size} bytes")

            # Verify content
            with open(output_file, 'r') as f:
                content = f.read()
                if '"slideshow"' in content:
                    print("✅ Content verification passed")
                    return True
                else:
                    print("❌ Content verification failed")
                    return False
        else:
            print("❌ Output file not found")
            return False

    except subprocess.TimeoutExpired:
        print("❌ Download timed out")
        return False
    except Exception as e:
        print(f"❌ Error: {e}")
        return False
    finally:
        # Cleanup
        if os.path.exists(output_file):
            os.remove(output_file)

def test_multiple_downloads():
    """Test multiple downloads"""
    print("\nTesting multiple downloads...")

    urls = [
        ("https://httpbin.org/uuid", "uuid.json"),
        ("https://httpbin.org/ip", "ip.json"),
        ("https://httpbin.org/user-agent", "user-agent.json")
    ]

    success_count = 0

    for url, filename in urls:
        print(f"  Downloading {filename}...")
        cmd = [f"./bin/falcon-cli", url, "-o", filename]

        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)

            if result.returncode == 0 and os.path.exists(filename):
                success_count += 1
                print(f"    ✅ {filename}")
            else:
                print(f"    ❌ {filename}")
        except:
            print(f"    ❌ {filename}")
        finally:
            if os.path.exists(filename):
                os.remove(filename)

    print(f"\n  Results: {success_count}/{len(urls)} downloads successful")
    return success_count == len(urls)

def test_resume():
    """Test resume functionality"""
    print("\nTesting resume functionality...")

    output_file = "resume_test.bin"

    # First attempt - start download and cancel quickly
    cmd = [f"./bin/falcon-cli", "https://httpbin.org/bytes/1024", "-o", output_file]
    proc = subprocess.Popen(cmd)

    # Let it download for a moment
    time.sleep(0.2)

    # Cancel
    proc.terminate()
    proc.wait()

    # Check partial file
    partial_size = 0
    if os.path.exists(output_file):
        partial_size = os.path.getsize(output_file)
        print(f"  Partial download: {partial_size} bytes")

    # Second attempt - resume
    cmd = [f"./bin/falcon-cli", "https://httpbin.org/bytes/1024", "-o", output_file]

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)

        if result.returncode == 0 and os.path.exists(output_file):
            final_size = os.path.getsize(output_file)
            print(f"  Final download: {final_size} bytes")

            if final_size == 1024:
                print("  ✅ Resume functionality working")
                return True
            else:
                print("  ❌ Resume functionality failed")
                return False
        else:
            print("  ❌ Resume test failed")
            return False
    except:
        print("  ❌ Resume test error")
        return False
    finally:
        if os.path.exists(output_file):
            os.remove(output_file)

def main():
    """Main test runner"""
    print("=== Falcon Downloader Integration Tests ===\n")

    # Change to build directory
    if os.path.exists("build"):
        os.chdir("build")
    elif os.path.basename(os.getcwd()) != "build":
        print("Error: Please run from project root or build directory")
        sys.exit(1)

    # Check if falcon-cli exists
    if not os.path.exists("bin/falcon-cli"):
        print("Error: falcon-cli not found in build/bin/")
        print("Please run 'cmake --build build --target falcon-cli' first")
        sys.exit(1)

    # Run tests
    results = []

    results.append(("Basic HTTP Download", test_http_download()))
    results.append(("Multiple Downloads", test_multiple_downloads()))
    results.append(("Resume Functionality", test_resume()))

    # Summary
    print("\n=== Test Summary ===")
    passed = 0
    for test_name, result in results:
        status = "✅ PASS" if result else "❌ FAIL"
        print(f"{test_name:25} {status}")
        if result:
            passed += 1

    print(f"\nOverall: {passed}/{len(results)} tests passed")

    if passed == len(results):
        print("\n🎉 All tests passed!")
        return 0
    else:
        print("\n⚠️  Some tests failed")
        return 1

if __name__ == "__main__":
    sys.exit(main())