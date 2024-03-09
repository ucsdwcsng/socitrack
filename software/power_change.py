stock_power = [0x67, 0x8B, 0x85]
stock_fine_gain = [0b00111, 0b01011, 0b00101]

filepath = "squarepoint/firmware/dw1000.c"

while True:
    print("How much change in fine gain? [-5, 20]")
    try:
        fine_gain_change = int(input())
    except ValueError:
        print("Invalid input")
        continue

    if fine_gain_change not in range(-5, 21):
        print("Input value not in range:", fine_gain_change)
    break
    
for i in range(3):
    stock_fine_gain[i] += fine_gain_change
    # Modify stock_power[i] based on stock_fine_gain[i], which consists of last 5 bits
    stock_power[i] = (stock_power[i] & 0b11100000) | stock_fine_gain[i]

print()

# Output the new stock_power in hex
output = ""
print("New stock_power:")
for i in range(3):
    string = f"{stock_power[i]:X}"

    print("0x" + string * 4 + "UL", end=(", " if i != 2 else "\n"))
    output += "0x" + string * 4 + "UL" + (", " if i != 2 else "")

# Change line 42 in dw1000.c
with open(filepath, "r") as file:
    lines = file.readlines()
    lines[41] = f"const uint32_t tx_power[3] = {{ {output} }};\n"
    

with open(filepath, "w") as file:
    file.writelines(lines)
    print("dw1000.c has been modified")