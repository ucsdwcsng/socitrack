MAC="c0:98:e5:42:00:"

if [ -z "$1" ]; then
    echo "What MAC to flash?"
    read MAC_ID
    echo "Flashing to STM, prepare the JTAG and press enter when ready."
    read temp
else
    echo "Flashing to $1"
    MAC_ID=$1
    echo "Flashing STM..."
fi

cd squarepoint
# make clean
make -j4 flash ID=$MAC$MAC_ID

echo "Flashing to nRF, prepare the JTAG and press enter when ready."
read temp

#sleep 1

cd ../tottag
# make clean
make -j8 BOARD_REV=H DEBUG_MODE=1 FORCE_RTC_RESET=1 flash ID=$MAC$MAC_ID
make -j8 BOARD_REV=H DEBUG_MODE=1 flash ID=$MAC$MAC_ID
