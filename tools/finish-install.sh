#!/bin/bash
# LING OS finish-install helper: completes an interrupted install whose /target copy is mostly done.
# Run as root from the live ISO: bash /tmp/finish-install.sh
set -u
log() { echo "[finish] $*"; }
err() { echo "[finish][ERROR] $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || err "run as root (sudo -i)"
[ -d /target/etc ] || err "/target is not mounted"
[ -f /target/etc/lingos-image-info ] || err "target copy incomplete: /target/etc/lingos-image-info missing"

ROOT_DEV=$(findmnt -n -o SOURCE /target | head -1)
[ -n "$ROOT_DEV" ] || err "cannot detect target root device"
ROOT_UUID=$(blkid -s UUID -o value "$ROOT_DEV" 2>/dev/null)
[ -n "$ROOT_UUID" ] || err "cannot read root UUID of $ROOT_DEV"
DISK=$(lsblk -no PKNAME "$ROOT_DEV" 2>/dev/null | head -1)
[ -n "$DISK" ] || err "cannot detect parent disk of $ROOT_DEV"
EFI_DEV=$(lsblk -no NAME,PARTTYPENAME "/dev/$DISK" 2>/dev/null | awk '$2 ~ /EFI/ {print "/dev/"$1; exit}')
[ -n "$EFI_DEV" ] || EFI_DEV="/dev/${DISK}1"
[ -b "$EFI_DEV" ] || err "EFI partition not found (expected $EFI_DEV)"

mkdir -p /target/boot/efi
if ! mountpoint -q /target/boot/efi; then
    mount "$EFI_DEV" /target/boot/efi || err "cannot mount EFI $EFI_DEV"
fi
EFI_UUID=$(blkid -s UUID -o value "$EFI_DEV" 2>/dev/null)
[ -n "$EFI_UUID" ] || err "cannot read EFI UUID"

cat > /target/etc/fstab <<EOF
UUID=$ROOT_UUID / ext4 defaults 0 1
UUID=$EFI_UUID /boot/efi vfat umask=0077 0 1
EOF
chmod 644 /target/etc/fstab

rm -f /target/etc/lingos-live
rm -f /target/etc/systemd/system/lingos-installer.service
rm -f /target/etc/systemd/system/multi-user.target.wants/lingos-installer.service
rm -f /target/etc/systemd/system/multi-user.target.wants/lingos-oobe.service
rm -rf /target/etc/systemd/system/getty@tty1.service.d
ln -sf /etc/systemd/system/lingos.service /target/etc/systemd/system/multi-user.target.wants/lingos.service 2>/dev/null || true

if ! grep -q '^lingos:' /target/etc/passwd; then
    chroot /target useradd -m -s /bin/bash -G video,render,input,audio lingos 2>/dev/null \
        || chroot /target useradd -m -s /bin/bash lingos 2>/dev/null || true
fi
printf 'lingos:live\n' | chroot /target chpasswd 2>/dev/null || true

mkdir -p /target/etc/sudoers.d /target/etc/lightdm/lightdm.conf.d /target/etc/lingos /target/root
printf 'lingos ALL=(root) NOPASSWD: /usr/local/sbin/lingos-oobe-apply\n' > /target/etc/sudoers.d/90-lingos-oobe
chmod 0440 /target/etc/sudoers.d/90-lingos-oobe
printf '[Seat:*]\nautologin-user=lingos\nautologin-user-timeout=0\nuser-session=lingos-oobe\n' \
    > /target/etc/lightdm/lightdm.conf.d/60-lingos-oobe.conf
chmod 0644 /target/etc/lightdm/lightdm.conf.d/60-lingos-oobe.conf
printf '{"schema_version":1,"privacy_level":"L1","local_only":true,"flags":{}}\n' \
    > /target/etc/lingos/security-policy.json
chmod 600 /target/etc/lingos/security-policy.json
printf 'lingos\n' > /target/etc/hostname
printf 'live\n' > /target/root/lingos-oobe-password.txt
chmod 600 /target/root/lingos-oobe-password.txt
ln -sf /lib/systemd/system/lightdm.service /target/etc/systemd/system/display-manager.service

for d in dev proc sys; do
    mountpoint -q "/target/$d" || mount --bind "/$d" "/target/$d"
done

ARCH=$(uname -m)
case "$ARCH" in
    aarch64|arm64) GRUBTARGET=arm64-efi ;;
    x86_64|amd64)  GRUBTARGET=x86_64-efi ;;
    *) err "unsupported arch $ARCH" ;;
esac
chroot /target grub-install --target="$GRUBTARGET" --efi-directory=/boot/efi --bootloader-id=LINGOS \
    || err "grub-install failed"
chroot /target update-grub >/tmp/finish-grub.log 2>&1 || err "update-grub failed (see /tmp/finish-grub.log)"
chroot /target update-initramfs -u -k all >/tmp/finish-initramfs.log 2>&1 || log "WARN: update-initramfs failed (see /tmp/finish-initramfs.log)"
chroot /target update-grub >/dev/null 2>&1 || true
echo "[finish] DONE. Reboot and select LINGOS in the Vectras boot menu."
echo "[finish] Temporary password: lingos / live ; first boot should run OOBE."
