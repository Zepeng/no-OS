#!/usr/bin/env python3
"""
Simple UART Data Receiver - Save raw data to TXT file
No parsing, just save everything received to a text file
"""

import serial
import serial.tools.list_ports
import time
import sys
from datetime import datetime
import argparse
import os

class SimpleUARTReceiver:
    def __init__(self, port=None, baudrate=115200, txt_filename=None):
        self.port = port
        self.baudrate = baudrate
        self.ser = None
        self.running = False
        self.txt_filename = txt_filename or f"uart_data_{datetime.now().strftime('%Y%m%d_%H%M%S')}.txt"
        self.txt_file = None
        
        # Statistics
        self.lines_received = 0
        self.bytes_received = 0
        self.start_time = None
        
    def list_available_ports(self):
        """List all available serial ports"""
        ports = serial.tools.list_ports.comports()
        if not ports:
            print("No serial ports found!")
            return []
        
        print("Available serial ports:")
        for i, port in enumerate(ports):
            print(f"  {i}: {port.device} - {port.description}")
        return ports
    
    def auto_detect_port(self):
        """Try to auto-detect the correct port"""
        ports = serial.tools.list_ports.comports()
        
        # Look for common FPGA/development board descriptors
        keywords = ['uart', 'usb', 'serial', 'ftdi', 'cp210', 'ch340', 'cdc']
        
        for port in ports:
            description = port.description.lower()
            if any(keyword in description for keyword in keywords):
                print(f"Auto-detected port: {port.device} ({port.description})")
                return port.device
        
        # If no specific match, return first available port
        if ports:
            print(f"Using first available port: {ports[0].device}")
            return ports[0].device
        
        return None
    
    def connect(self, port=None):
        """Connect to serial port"""
        if port:
            self.port = port
        elif not self.port:
            self.port = self.auto_detect_port()
        
        if not self.port:
            print("No port specified and none auto-detected!")
            return False
        
        try:
            self.ser = serial.Serial(
                port=self.port,
                baudrate=self.baudrate,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=1,
                xonxoff=False,
                rtscts=False,
                dsrdtr=False
            )
            
            print(f"✓ Connected to {self.port} at {self.baudrate} baud")
            return True
            
        except serial.SerialException as e:
            print(f"✗ Failed to connect to {self.port}: {e}")
            return False
        except Exception as e:
            print(f"✗ Unexpected error: {e}")
            return False
    
    def setup_txt_file(self):
        """Setup TXT file for writing"""
        try:
            # Open file with line buffering for immediate writing
            self.txt_file = open(self.txt_filename, 'w', encoding='utf-8', buffering=1)
            
            # Write header with timestamp
            header = f"UART Data Log - Started: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n"
            header += f"Port: {self.port}, Baud Rate: {self.baudrate}\n"
            header += "=" * 60 + "\n"
            self.txt_file.write(header)
            self.txt_file.flush()
            
            print(f"✓ TXT file created: {self.txt_filename}")
            print(f"✓ Full path: {os.path.abspath(self.txt_filename)}")
            return True
            
        except Exception as e:
            print(f"✗ Failed to create TXT file: {e}")
            return False
    
    def print_statistics(self):
        """Print reception statistics"""
        if self.start_time:
            elapsed = time.time() - self.start_time
            lines_per_sec = self.lines_received / elapsed if elapsed > 0 else 0
            bytes_per_sec = self.bytes_received / elapsed if elapsed > 0 else 0
            
            print(f"\r📊 Lines: {self.lines_received:6d} | "
                  f"Bytes: {self.bytes_received:8d} | "
                  f"Rate: {lines_per_sec:6.1f} lines/sec | "
                  f"{bytes_per_sec:8.1f} bytes/sec | "
                  f"Time: {elapsed:6.1f}s", end='', flush=True)
    
    def receive_data(self, duration=None, show_data=True, show_stats=True, add_timestamps=True):
        """Receive raw data from UART and save to TXT file"""
        if not self.ser or not self.ser.is_open:
            print("Serial port not connected!")
            return False
        
        if not self.setup_txt_file():
            return False
        
        self.running = True
        self.start_time = time.time()
        last_stats_time = time.time()
        
        print(f"📡 Receiving data... Press Ctrl+C to stop")
        print(f"💾 Saving to: {self.txt_filename}")
        if add_timestamps:
            print("⏰ Adding timestamps to each line")
        print("-" * 80)
        
        try:
            while self.running:
                # Check if duration specified and exceeded
                if duration and (time.time() - self.start_time) > duration:
                    break
                
                # Read line from serial port
                try:
                    if self.ser.in_waiting > 0:
                        raw_line = self.ser.readline()
                        line = raw_line.decode('utf-8', errors='ignore').rstrip('\r\n')
                        
                        if line:  # Only process non-empty lines
                            self.lines_received += 1
                            self.bytes_received += len(raw_line)
                            
                            # Add timestamp if requested
                            if add_timestamps:
                                timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]
                                file_line = f"[{timestamp}] {line}\n"
                                display_line = f"[{timestamp}] {line}"
                            else:
                                file_line = f"{line}\n"
                                display_line = line
                            
                            # Write to file immediately
                            self.txt_file.write(file_line)
                            
                            # Flush every 10 lines to ensure data is written
                            if self.lines_received % 10 == 0:
                                self.txt_file.flush()
                            
                            # Show data if requested
                            if show_data:
                                print(display_line)
                            
                            # Update statistics periodically
                            if show_stats and not show_data:
                                current_time = time.time()
                                if current_time - last_stats_time >= 1.0:  # Update every second
                                    self.print_statistics()
                                    last_stats_time = current_time
                    
                    else:
                        time.sleep(0.001)  # Small delay to prevent CPU spinning
                        
                except UnicodeDecodeError as e:
                    error_msg = f"⚠️  Unicode decode error: {e}\n"
                    print(error_msg.strip())
                    self.txt_file.write(error_msg)
                    continue
                
        except KeyboardInterrupt:
            print(f"\n🛑 Stopping data reception...")
        
        except Exception as e:
            print(f"\n❌ Error during reception: {e}")
        
        finally:
            self.stop()
            
        # Final flush to ensure all data is written
        if self.txt_file and not self.txt_file.closed:
            self.txt_file.flush()
            
        # Final statistics
        if show_stats:
            print(f"\n📈 Final Statistics:")
            print(f"   Total lines received: {self.lines_received}")
            print(f"   Total bytes received: {self.bytes_received}")
            if self.start_time:
                elapsed = time.time() - self.start_time
                lines_rate = self.lines_received / elapsed if elapsed > 0 else 0
                bytes_rate = self.bytes_received / elapsed if elapsed > 0 else 0
                print(f"   Average rate: {lines_rate:.1f} lines/second, {bytes_rate:.1f} bytes/second")
                print(f"   Total time: {elapsed:.1f} seconds")
            print(f"   Data saved to: {os.path.abspath(self.txt_filename)}")
            
            # Check file size
            try:
                file_size = os.path.getsize(self.txt_filename)
                print(f"   File size: {file_size} bytes")
                if file_size == 0:
                    print("   ⚠️  WARNING: TXT file is empty!")
                else:
                    print(f"   ✓ File contains data ({file_size} bytes)")
            except Exception as e:
                print(f"   ⚠️  WARNING: Could not check file size: {e}")
        
        return True
    
    def stop(self):
        """Stop data reception and cleanup"""
        self.running = False
        
        if self.txt_file and not self.txt_file.closed:
            # Write footer
            footer = f"\n" + "=" * 60 + f"\nLog ended: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n"
            self.txt_file.write(footer)
            self.txt_file.flush()
            self.txt_file.close()
            self.txt_file = None
        
        if self.ser and self.ser.is_open:
            self.ser.close()
        
        print(f"✓ Connection closed and data saved")

def main():
    parser = argparse.ArgumentParser(description='Simple UART Data Receiver (TXT output)')
    parser.add_argument('-p', '--port', help='Serial port (e.g., COM3, /dev/ttyACM0)')
    parser.add_argument('-b', '--baudrate', type=int, default=115200, help='Baud rate (default: 115200)')
    parser.add_argument('-o', '--output', help='Output TXT filename')
    parser.add_argument('-d', '--duration', type=float, help='Duration in seconds (default: infinite)')
    parser.add_argument('-q', '--quiet', default=True, action='store_true', help='Don\'t show received data, only statistics')
    parser.add_argument('--no-timestamps', default=False, action='store_true', help='Don\'t add timestamps to lines')
    parser.add_argument('-l', '--list', action='store_true', help='List available ports and exit')
    
    args = parser.parse_args()
    
    # List ports and exit if requested
    if args.list:
        receiver = SimpleUARTReceiver()
        receiver.list_available_ports()
        return
    
    # Create receiver instance
    receiver = SimpleUARTReceiver(
        port=args.port,
        baudrate=args.baudrate,
        txt_filename=args.output
    )
    
    # If no port specified, list available ports and let user choose
    if not args.port:
        ports = receiver.list_available_ports()
        if not ports:
            print("No serial ports available!")
            return
        
        if len(ports) == 1:
            selected_port = ports[0].device
            print(f"Auto-selecting only available port: {selected_port}")
        else:
            try:
                choice = input("Enter port number or press Enter for auto-detection: ").strip()
                if choice:
                    idx = int(choice)
                    selected_port = ports[idx].device
                else:
                    selected_port = receiver.auto_detect_port()
            except (ValueError, IndexError):
                print("Invalid selection!")
                return
        
        receiver.port = selected_port
    
    # Connect and start receiving
    if receiver.connect():
        try:
            receiver.receive_data(
                duration=args.duration,
                show_data=not args.quiet,
                show_stats=True,
                add_timestamps=not args.no_timestamps
            )
        except KeyboardInterrupt:
            print("\nExiting...")
        finally:
            receiver.stop()
    else:
        print("Failed to connect to serial port!")

if __name__ == "__main__":
    main()

# USAGE EXAMPLES:
"""
# Basic usage (auto-detect port, save with timestamps):
python simple_uart_receiver.py

# Specify port:
python simple_uart_receiver.py -p /dev/ttyACM0        # Linux
python simple_uart_receiver.py -p COM3               # Windows

# Custom filename:
python simple_uart_receiver.py -o my_uart_log.txt

# Run for specific duration:
python simple_uart_receiver.py -d 60                 # Run for 60 seconds

# Quiet mode (only statistics):
python simple_uart_receiver.py -q

# No timestamps in file:
python simple_uart_receiver.py --no-timestamps

# List available ports:
python simple_uart_receiver.py -l
"""
