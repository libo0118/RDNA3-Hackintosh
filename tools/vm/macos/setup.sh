#!/bin/sh
# RDNA3-Hackintosh VM test: one-time setup inside the macOS VM.
# Run in Terminal (the host's "rdna3vm window" serves it):
#   curl -s http://10.0.2.2:8000/setup.sh | sudo sh
# Installs the kexts (firmware inside), the report tools and a boot-time
# daemon that collects a report ~90 s after every boot, uploads it to the
# host and, when the host asked for it (boot-arg rdna3vmauto=1), shuts the
# VM down. Run it again after every new build.
set -eu
HOST=http://10.0.2.2:8000
D="/Library/Application Support/RDNA3VM"
[ "$(id -u)" = 0 ] || { echo "run it with sudo"; exit 1; }

echo "== fetching the bundle from the host"
rm -rf "$D"
mkdir -p "$D"
curl -sf "$HOST/rdna3-bundle.tar.gz" | tar -xz -C "$D"

echo "== kexts -> /Library/Extensions"
for k in "$D"/kexts/*.kext; do
    n=$(basename "$k")
    rm -rf "/Library/Extensions/$n"
    cp -R "$k" /Library/Extensions/
    chown -R root:wheel "/Library/Extensions/$n"
    chmod -R go-w "/Library/Extensions/$n"
    chmod 755 "/Library/Extensions/$n/Contents/MacOS/"* 2>/dev/null || true
    # ad-hoc signature over the final bundle (firmware included): without one
    # kernelmanagerd refuses it ("invalid codeful kernel extension"), and SIP
    # off lets an ad-hoc signed kext into the Auxiliary KC
    codesign --force --deep --sign - "/Library/Extensions/$n" >/dev/null 2>&1 ||
        echo "   WARNING: codesign failed for $n"
    echo "   $n ($(codesign -dv "/Library/Extensions/$n" 2>&1 | grep -o 'Signature=.*' || echo unsigned))"
done
chmod +x "$D/bin/"* "$D/tools/macos/"*.sh "$D/autoreport.sh" 2>/dev/null || true

echo "== report daemon"
cat > /Library/LaunchDaemons/org.rdna3.vmreport.plist <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>org.rdna3.vmreport</string>
    <key>ProgramArguments</key>
    <array><string>/bin/sh</string><string>$D/autoreport.sh</string></array>
    <key>RunAtLoad</key><true/>
    <key>StandardOutPath</key><string>/var/log/rdna3-vmreport.log</string>
    <key>StandardErrorPath</key><string>/var/log/rdna3-vmreport.log</string>
</dict>
</plist>
PLIST
chown root:wheel /Library/LaunchDaemons/org.rdna3.vmreport.plist
chmod 644 /Library/LaunchDaemons/org.rdna3.vmreport.plist

csrutil status | grep -q disabled || echo "WARNING: SIP is not disabled (csrutil status). Run 'rdna3vm prepare' on the host again."

echo "== rebuilding the kext collection (takes a moment)"
kmutil install --update-all || true
echo
echo "Done. If macOS shows 'System Extension Blocked':"
echo "System Settings -> Privacy & Security -> Allow, then restart."
echo "Now shut the VM down:  sudo shutdown -h now"
