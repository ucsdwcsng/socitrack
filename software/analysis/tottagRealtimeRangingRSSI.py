#!/usr/bin/env python


# PYTHON INCLUSIONS ---------------------------------------------------------------------------------------------------

import asyncio
import functools
import struct
import os, sys
from bleak import BleakClient, BleakScanner
from datetime import datetime


# CONSTANTS AND DEFINITIONS -------------------------------------------------------------------------------------------

EUI_LENGTH = 1
RANGE_DATA_LENGTH = EUI_LENGTH + 4
TOTTAG_DATA_UUID = 'd68c3153-a23f-ee90-0c45-5231395e5d2e'

# No more than 30 RSSIs per device
NUM_RSSI_ENTRIES = 1


# STATE VARIABLES -----------------------------------------------------------------------------------------------------

filename_base = datetime.now().strftime('ranging_data_%Y-%m-%d_%H-%M-%S_')
data_characteristics = []
ranging_files = []
tottags = []

cur_tags = {}

# HELPER FUNCTIONS AND CALLBACKS --------------------------------------------------------------------------------------

def data_received_callback(data_file, addr, sender_characteristic, data):
  identifier = data[0]
  # Get last 2 bytes of address
  if identifier == 14:
    num_ranges, from_eui = struct.unpack('<BB', data[1:3])
    timestamp = struct.unpack('<I', data[(3+num_ranges*5):(7+num_ranges*5)])[0]
    print('Range from Device {} to {} device(s) @ {}:'.format(hex(from_eui)[2:], num_ranges, timestamp))
    for i in range(num_ranges):
      to_eui, range_mm = struct.unpack('<BI', data[(3+i*5):(3+(i+1)*5)])
      print('\tDevice {} with millimeter range {}'.format(hex(to_eui)[2:], range_mm))
      # Get OS timestamp in seconds
      osTime = int(datetime.now().timestamp())
      cur_tags[addr]=({'to': to_eui, 'from': from_eui, 'range': range_mm, 'timestamp': timestamp, 'ostime': osTime, 'num_ranges': num_ranges})
  elif identifier == 15:
    if len(cur_tags) == 0 or len(data[1:]) < 120:
      return
    # Get RSSI data entries starting from 2nd byte as list of floats
    rssi_fmt = '<' + 'f'*cur_tags[addr]['num_ranges']*30
    rssi_struct = struct.unpack(rssi_fmt, data[1:121])
    # data_file.write('{}\t{}\t{}\t{}\t{}'.format(osTime, timestamp, hex(from_eui)[2:], hex(to_eui)[2:], range_mm))
    data_file.write('{}\t{}\t{}\t{}\t{}'.format(cur_tags[addr]['ostime'], cur_tags[addr]['timestamp'], hex(cur_tags[addr]['from'])[2:], hex(cur_tags[addr]['to'])[2:], cur_tags[addr]['range']))
    for i in range(NUM_RSSI_ENTRIES):
      # Write to file, truncate to 4 decimal places
      data_file.write('\t{}'.format(round(rssi_struct[i], 4)))
    data_file.write('\n')
  else:
    print('ERROR: Invalid data packet identifier: {}'.format(identifier))


# MAIN RANGE LOGGING FUNCTION -----------------------------------------------------------------------------------------

async def log_ranges():

  # Scan for TotTag devices for 6 seconds
  scanner = BleakScanner()
  await scanner.start()
  await asyncio.sleep(6.0)
  await scanner.stop()

  # Iterate through all discovered TotTag devices
  for device in scanner.discovered_devices:
    if device.name == 'TotTag':

      # Connect to the specified TotTag and locate the ranging data service
      print('Found Device: {}'.format(device.address))
      client = BleakClient(device, use_cached=False)
      try:
        await client.connect()
        for service in await client.get_services():
          for characteristic in service.characteristics:

            # Open a log file, register for data notifications, and add this TotTag to the list of valid devices
            if characteristic.uuid == TOTTAG_DATA_UUID:
              try:
                file = open(filename_base + client.address.replace(':', '') + '.data', 'w')
                file.write('UNIX\tTimestamp\tFrom\tTo\tDistance (mm)\n')
                for i in range(NUM_RSSI_ENTRIES):
                  file.write('\tRSSI_{}'.format(i))
                file.write('\n')
              except Exception as e:
                print(e)
                print('ERROR: Unable to create a ranging data log file')
                sys.exit('Unable to create a ranging data log file: Cannot continue!')
              await client.start_notify(characteristic, functools.partial(data_received_callback, file, client.address))
              data_characteristics.append(characteristic)
              ranging_files.append(file)
              tottags.append(client)

      except Exception as e:
        print('ERROR: Unable to connect to TotTag {}'.format(device.address))
        await client.disconnect()

  # Wait forever while ranging data is being logged
  while (True): await asyncio.sleep(1.0)

  # Disconnect from all TotTag devices
  for i in range(len(tottags)):
    await tottags[i].stop_notify(data_characteristics[i])
    await tottags[i].disconnect()


# TOP-LEVEL FUNCTIONALITY ---------------------------------------------------------------------------------------------

print('\nSearching 6 seconds for TotTags...\n')
loop = asyncio.get_event_loop()

# Listen forever for TotTag ranges
try:
  loop.run_until_complete(log_ranges())

# Gracefully close all log files
finally:
  for file in ranging_files:
    file.close()
