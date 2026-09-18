CpapDash for Raspberry Pi (32-bit Raspberry Pi OS, trixie)

    unzip hms-cpap-linux-armhf.zip
    cd hms-cpap-linux-armhf
    sudo ./install.sh

Then open the address it prints. The same three lines install a newer version.
Your config, database and card archive live in ~/.hms-cpap and are never touched.

Once installed, later versions can also be applied from the web UI (Settings,
Updates). The download is checked against the release's own checksums before
anything is replaced, and the previous version comes back if the new one does
not start.

To remove it: sudo systemctl disable --now hms-cpap hms-cpap-update.path, then
delete /usr/local/bin/hms_cpap, /usr/local/lib/hms-cpap,
/etc/systemd/system/hms-cpap*.service, /etc/systemd/system/hms-cpap-update.path
and ~/static.
