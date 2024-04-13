import os, time, subprocess, signal
from pynput.keyboard import Key, Controller, KeyCode

keyboard = Controller()

# Data Naming Scheme:
#   1st and 2nd word tells orientation
#   3rd word tells gain
base_folder = "quick_testing/Forward/"

data_folders = [
    "F_Ba_0dB/",
    "L_Ba_0dB/",
    "R_Ba_0dB/",
    "Ba_Ba_0dB/",
    "T_Ba_0dB/",
    "T_T_0dB/"
]

uwb_folder = "~/Research/wcsng-socitrack/software"
vr_folder = "~/Research/uloc/data_collection_template/host/vr/data"
uwb_folder = os.path.expanduser(uwb_folder)
vr_folder = os.path.expanduser(vr_folder)

terminal_handler = "gnome-terminal"
# terminal_handler = "terminator"

term1_cmd = ""
term2_cmd = ""

# Term 1 handles collecting VR data
term1_cmd_list = [
    "sleep 5",
    "python3 ~/Research/uloc/data_collection_template/host/vr/collect_vr.py"
]
term1_cmd = "; ".join(term1_cmd_list)

# Term 2 handles collecting UWB data
term2_cmd_list = [
    "python3 ~/Research/wcsng-socitrack/software/analysis/tottagRealtimeRangingRSSI.py"
]
term2_cmd = "; ".join(term2_cmd_list)

def countdown(val):
    temp = val
    while temp >= 0:
        print(temp, end=(', ' if temp > 0 else ''), flush=True)
        time.sleep(1)
        temp -= 1
    print('\n\n\n\n')

for index,test in enumerate(data_folders):
    processes = []
    data_folder = os.path.join(base_folder, test)
    print("Testing: {}".format(data_folder))
    time.sleep(5)

    # Run commands in separate terminals
    p1 = subprocess.Popen([terminal_handler, '--profile=Big', '--wait','-e', "bash -c \"{}\"".format(term1_cmd)])
    p2 = subprocess.Popen([terminal_handler, '--profile=Big', '--wait','-e', "bash -c \"{}\"".format(term2_cmd)])
    processes = [p1, p2]
    print(p1.pid)
    print(p2.pid)

    time.sleep(1)
    keyboard.press(Key.cmd); keyboard.press(Key.left);keyboard.release(Key.cmd); keyboard.release(Key.left);
    time.sleep(0.5)
    keyboard.press(Key.alt); keyboard.press(Key.tab);keyboard.release(Key.alt); keyboard.release(Key.tab);
    time.sleep(0.5)
    keyboard.press(Key.cmd); keyboard.press(Key.right);keyboard.release(Key.cmd); keyboard.release(Key.right);

    # Wait for data collection to finish (limited to 95 seconds)
    time.sleep(105)
    keyboard.press(Key.ctrl); keyboard.press('c');keyboard.release(Key.ctrl); keyboard.release('c');
    time.sleep(0.5)
    keyboard.press(Key.ctrl); keyboard.press('c');keyboard.release(Key.ctrl); keyboard.release('c');

    # After data collection, data is located in different areas; move to specified folder
    if not os.path.exists(data_folder):
        vr_path = os.path.join(data_folder, "vr")
        uwb_path = os.path.join(data_folder, "uwb")
        os.makedirs(data_folder)
        os.makedirs(vr_path)
        os.makedirs(uwb_path)

    # Move VR data to specified folder

    # Find latest VR data file
    vr_files = os.listdir(vr_folder)
    vr_file_paths = [os.path.join(vr_folder, file) for file in vr_files]
    latest_vr_file = max(vr_file_paths, key=os.path.getctime)

    vr_file_index = vr_file_paths.index(latest_vr_file)

    latest_vr_file_path = latest_vr_file
    latest_vr_file_dest = os.path.join(data_folder, "vr", vr_files[vr_file_index])

    # Move latest VR data file to specified folder
    os.system("mv {} {}".format(latest_vr_file_path, latest_vr_file_dest))

    # Move UWB data to specified folder
    uwb_files = os.listdir(".")

    # Find files that start with "ranging"
    ranging_files = [f for f in uwb_files if f.startswith("ranging")]

    # Move ranging files to specified folder
    for f in ranging_files:
        os.system("mv {} {}".format(f, os.path.join(data_folder, "uwb", f)))
    print("\n\n\n\nWait time to reset tags for bluetooth")
    if index + 2 < len(data_folders):
        print(f'Next Test: {data_folders[index+1]}')
    else:
        print(f'Last Test: {data_folders[index+1]}')
    countdown(50)
