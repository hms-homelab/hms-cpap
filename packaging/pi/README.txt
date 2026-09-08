CpapDash for Raspberry Pi (32-bit Raspberry Pi OS, trixie)

    unzip hms-cpap-linux-armhf.zip
    cd hms-cpap-linux-armhf
    sudo ./install.sh

Then open the address it prints. The same three lines install a newer version.
Your config, database and card archive live in ~/.hms-cpap and are never touched.

To remove it: sudo systemctl disable --now hms-cpap, then delete
/usr/local/bin/hms_cpap, /etc/systemd/system/hms-cpap.service and ~/static.
