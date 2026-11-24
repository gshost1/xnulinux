# xnulinux on a rented cloud ARM VM, without a GPU

This is a how-to for running xnulinux on an arm64 VM from a cloud provider: no Apple hardware under it, no GPU, nobody
at its screen. It records what was done on one such VM, in the order that worked, with the failures met on the way.

It is not the only non-Apple host. Upstream xnulinux already runs on a bare-metal NVIDIA DGX Spark (see the main
[README](../README.md)), and macOS-DroidVM, UniXNU and Snapintosh run macOS on other non-Apple ARM machines in other
ways. This guide only adds the cloud case to xnulinux: a host whose memory map, timer and CPU differ from a Mac's,
reached only over ssh.

If you want an AI coding agent to do the work, give it [`cloud-arm-agent.md`](cloud-arm-agent.md) instead. The
procedure is the same.

**Before you start, two things to decide yourself.**

- **Apple's licence.** Apple's software licence for macOS restricts it to Apple-branded hardware. The SDKs and the
  Kernel Debug Kit used to build the kernel have similar terms. Whether to go on is your decision. This repository
  contains no Apple files, and nothing here should be read as legal advice.
- **Money.** A cloud VM, a rented Mac (if you do not own one), storage and network transfer all cost money. Section 1
  says what was used. Check your provider's prices first.

## Contents

1. What you need
2. How the pieces fit
3. Prepare the VM
4. Build the kernel on the VM
5. Reserve memory for the hosted kernel
6. The settings file
7. Build the disk on a Mac
8. Move the disk to the VM
9. Boot and stop
10. Reach the hosted system
11. Tuning for CPU rendering
12. Check the result
13. Measured numbers
14. Troubleshooting
15. What does not work, and what was not tested
16. Saving your work

Commands are marked with where they run: **VM** (the cloud VM), **Mac**, **laptop** (your own machine) or **guest**
(inside the hosted macOS).

## 1. What you need

- **An Apple silicon Mac, for a few hours.** This is required, and there is no way around it here. The system disk is
  made by Apple's own installer inside Apple's Virtualization framework, which only runs on Apple silicon, and the
  Metal plugin that draws the desktop is built with Apple's developer tools. The Mac needs macOS 27, about 80 GB free,
  and a user who can log in at its screen (a rented Mac needs automatic login, section 7). A Mac that is itself a
  virtual machine will not do.
- **An arm64 cloud VM.** What was used:
  - Google Cloud **C4A** (`c4a-highmem-4`): Arm **Neoverse V2**, **4 vCPU, 32 GB RAM**, 96 GB disk.
  - **Ubuntu 26.04 LTS** arm64, kernel **7.0.0-1017-gcp**, 4 KB pages.
  - CPU features that matter: pointer authentication (`paca` and `pacg` in `/proc/cpuinfo`). This CPU also faults
    on a failed authentication (FEAT_FPAC), which is why one of the fixes in this branch was needed.
  - The counter runs at 1 GHz (a Mac's runs at 24 MHz). Section 6 deals with that.
  - Root access, Secure Boot off, kernel lockdown off, and the right to load unsigned kernel modules.
  Any arm64 VM with pointer authentication, 4 KB pages and at least 16 GB of RAM should be a candidate, but only the
  one above was tested. The memory numbers in section 5 are for 32 GB.
- **A way to move about 30 GB** from the Mac to the VM: an object-storage bucket, or plain ssh if the two are close.
- **Your own machine** with ssh, to drive both.
- A macOS 27 restore image (`.ipsw`) from Apple. The build used here is **macOS 27.2 (26B123)**. The Mac downloads it.

Rough time and cost, where it was measured:

| Step | Measured |
|---|---|
| Kernel build on the VM, first time | About 55 minutes of VM time, including four failed tries that the fixes in this branch now avoid. A rebuild after a patch takes 25 to 35 seconds. |
| Disk and GPU bundle on the Mac | 8 minutes once the guest was installed and the tools were there. Installing the guest (a download of about 25 GB plus Apple's installer) was not timed. |
| Mac to bucket, 23 GB | 40 seconds to pack, 206 seconds to upload (Mac and bucket in the same region). |
| Bucket to VM, 28 GB | About 10 minutes; the VM's disk was the slow part. |
| Boot to desktop | About 1 minute after the hosted root shell appears. |
| Storage | About 2.30 US dollars a month for 100 GB in one bucket. |
| Network transfer, 29 GB between continents | Estimated at 1.50 to 2.50 US dollars. Not confirmed on a bill. |

The VM's hourly price and the Mac rental were not recorded: look them up.

## 2. How the pieces fit

- `xnu.ko` is the macOS kernel (XNU, built from Apple's open source) as a Linux kernel module. `fki.ko` is the small
  module that lends it CPUs, memory, a disk file and a network link.
- The hosted kernel needs one block of memory that Linux never touches. On a VM you reserve it on the kernel command
  line (section 5).
- The system disk is a file, `mac.img`, made on the Mac from a normal macOS install. It comes with two small files:
  `mac.conf` (two volume identifiers) and `trustcache.bin`.
- With no GPU, everything on screen is drawn on the CPU by Mesa's lavapipe, inside every process that draws.
- The desktop is a framebuffer that the VM dumps to a file. You look at it by converting the dump to a PNG, and you
  type and click by appending events to another file.

## 3. Prepare the VM

1. Create the VM (4 vCPU, 32 GB, arm64, Ubuntu 26.04, 100 GB disk) with your provider's tools and ssh into it.

2. **VM.** Check that the host can do it at all:

   ```sh
   grep -c paca /proc/cpuinfo        # expect: the number of CPUs (4)
   getconf PAGESIZE                  # expect: 4096
   uname -r                          # used here: 7.0.0-1017-gcp
   cat /sys/kernel/security/lockdown # expect: [none] integrity confidentiality
   ```

   If `paca` is missing or the page size is not 4096, stop: this host will not work.

3. **VM.** Get this repository, at the path the build scripts expect:

   ```sh
   mkdir -p ~/w && cd ~/w
   git clone --branch cloud-arm https://github.com/gshost1/xnulinux.git poc
   ```

   Check: `ls ~/w/poc/scripts/vm-xnu-build.sh` prints the path.

4. **Laptop.** Clone the same branch and set up the helper tools (they are described in
   [`tools/cloud/README.md`](../tools/cloud/README.md)):

   ```sh
   git clone --branch cloud-arm https://github.com/gshost1/xnulinux.git
   cd xnulinux
   cp tools/cloud/cloud.env.example tools/cloud/cloud.env
   ```

   Edit `tools/cloud/cloud.env`: set `VM_HOST` (the VM's address), `VM_USER` and `VM_KEY`. Then:

   ```sh
   tools/cloud/vm-ssh 'uname -m'     # expect: aarch64
   ```

## 4. Build the kernel on the VM

The build script downloads a toolchain from macOS-DroidVM, Apple's XNU source, and some Apple files that are not open
source: the Command Line Tools package (from Apple's own server), two SDKs and a Kernel Debug Kit. **As upstream ships
it, the SDKs and the Kernel Debug Kit come from third-party mirrors on GitHub**, pinned and checksummed in
macOS-DroidVM's lock file. Apple offers the same files to holders of a developer account. Decide whether that is
acceptable to you before you run it. The files stay on the VM.

1. **VM.** Run the build in the background and keep the log:

   ```sh
   cd ~/w/poc
   nohup bash scripts/vm-xnu-build.sh > ~/build.log 2>&1 &
   ```

   It takes tens of minutes. Watch it with `tail -f ~/build.log`.

2. **VM.** On a fresh VM the first pass hangs in a helper called `decomment` (see Troubleshooting, "the build
   stops making progress"). When the log stops moving and this prints a number above zero:

   ```sh
   pgrep -c -x decomment
   ```

   clean up and start the same script again:

   ```sh
   pkill -x decomment; pkill -f vm-xnu-build.sh
   find ~/w/dvm/out -name '*.strip.*' -delete; find ~/w/dvm/out -name '*.unifdef.*' -delete
   cd ~/w/poc && nohup bash scripts/vm-xnu-build.sh > ~/build2.log 2>&1 &
   ```

   Do this promptly: each stuck process writes gigabytes of temporary files, and they filled a 96 GB disk once.

3. **VM.** Check:

   ```sh
   ls -la ~/w/dvm/out/k27/kernel-k27.macho
   ```

   Expected: a file of about 24 MB.

4. **VM.** Get the kernel collection input, `kc27.bin`. It is the kernel cache of the virtual-Mac variant of macOS
   27.2, which holds the Apple kernel extensions the hosted system needs. macOS-DroidVM's tools fetch that one member
   from Apple's own update server and check it against Apple's manifest; the `ipsw` tool unpacks it.

   ```sh
   cd ~/w/dvm
   mkdir -p toolchains/ipsw artifacts/27.2 out/kc
   curl -fsSL -o ~/ipsw.tgz https://github.com/blacktop/ipsw/releases/download/v3.1.749/ipsw_3.1.749_linux_arm64.tar.gz
   tar -C toolchains/ipsw -xzf ~/ipsw.tgz
   python3 scripts/macos-inputs.py --profile configs/macos-27.2-26B123.json --output artifacts/27.2/inventory-26B123
   python3 scripts/macos-fetch-member.py KernelCache --profile configs/macos-27.2-26B123.json \
     --inventory artifacts/27.2/inventory-26B123/identities.json --identity vma2macosap --output-dir artifacts/27.2 --jobs 8
   toolchains/ipsw/ipsw img4 im4p extract -o out/kc/kc27.bin artifacts/27.2/kernelcache.release.vma2
   ls -la out/kc/kc27.bin
   ```

   Expected: `kc27.bin` of about 82 MB. (macOS-DroidVM's lock file pins an x86-64 build of `ipsw`; the arm64 build
   above is the one that runs here. Its one-step `product.py inputs` also downloads the whole OS, many GB, which is
   not needed.)

5. **VM.** Apply xnulinux's patches, build the kernel again with the kexts, and install the collection. The disk in
   this guide is macOS 27.2, so the code-signing platform version must be 27:

   ```sh
   cd ~/w/poc
   AMFI_PLATFORM=27 bash scripts/vm-xnu-rebuild.sh 2>&1 | tee ~/rebuild.log
   ls -la /lib/firmware/xnu/
   ```

   Expected: lines `applied 0001-...patch` and so on, `kernel: NN s`, `collection: NN s`, and both `kc.bin` (about
   90 MB) and `kernel.macho` in `/lib/firmware/xnu/`. Always build with `kc27.bin` in place: without it the kernel
   links with overlapping segments and traps early (Troubleshooting, "UBSan trap").

6. **VM.** Build the two modules against the running kernel:

   ```sh
   cd ~/w/poc
   make -s -C /lib/modules/$(uname -r)/build M=$PWD/fki-ko modules
   make -s -C /lib/modules/$(uname -r)/build M=$PWD/xnu-ko KBUILD_EXTRA_SYMBOLS=$PWD/fki-ko/Module.symvers modules
   ls -la fki-ko/fki.ko xnu-ko/xnu.ko
   ```

   Loading them prints "module verification failed ... tainting kernel". That is a warning.

## 5. Reserve memory for the hosted kernel

The hosted kernel needs about 8 GB as **one physically contiguous block**. Linux can sometimes find one on a freshly
booted host, but not reliably. The dependable way is to take the block away from Linux at boot (`mem=`) and hand it to
`fki.ko` as a "carve-out". The numbers depend on the VM's memory map.

On the 32 GB C4A VM, RAM runs from `0x40000000` to `0x83fffffff`, with firmware regions near the top. So:

- `mem=23G`: Linux uses RAM up to `0x5ffffffff`.
- carve-out: 8156 MB starting at `0x600000000`, ending below the firmware's region.
- `nokaslr`: otherwise the boot stub may place the Linux kernel image inside the carve-out on some boots.

A general-purpose reservation with `cma=8G` does **not** work here: on arm64 it is limited to memory below 4 GB.

1. **VM.** Install the settings and the boot-time service that loads `fki.ko` with the carve-out:

   ```sh
   cd ~/w/poc
   LINUX_MEM=23G CARVE_BASE=0x600000000 CARVE_MB=8156 bash scripts/vm-install-fki-service.sh
   sudo reboot
   ```

2. **VM.** After the reboot, check:

   ```sh
   cat /proc/cmdline                 # expect it to contain: mem=23G nokaslr log_buf_len=128M
   sudo dmesg | grep 'fki: .*MiB at' # expect: fki: 8156 MiB at 0x600000000 for the foreign kernel's DRAM (outside the host's memory)
   free -g | sed -n 2p               # expect about 22 GB total
   ```

For another VM size, read the memory map first (`sudo cat /proc/iomem | grep 'System RAM'`), keep the carve-out
inside RAM and clear of the top gigabyte, and set `LINUX_MEM` to end where the carve-out starts.

## 6. The settings file

`scripts/xnulinux-boot.sh` reads `/etc/xnulinux.conf`.

**VM.**

```sh
sudo tee /etc/xnulinux.conf >/dev/null <<'EOF'
CPUS=3
RAM_MB=7900
GPU=off
BOOTARGS="amfi_get_out_of_my_way=1 lp-sip0=2050 droidvm-timebase-hz=24000000 bt-transport-override=10"
EOF
```

Each line, and why:

| Setting | Why |
|---|---|
| `CPUS=3` | The hosted system takes Linux CPUs 0 to 2. The VM has 4; Linux needs one for itself and its device interrupts. 1, 2 and 3 were tried; 4 was not. |
| `RAM_MB=7900` | The hosted system's memory. It must fit inside the 8156 MB carve-out. |
| `GPU=off` | There is no GPU and no virtio-gpu device. Everything is drawn on the CPU. |
| `amfi_get_out_of_my_way=1 lp-sip0=2050` | Upstream's defaults: code-signing enforcement relaxed, as the disk is modified. They must be repeated because `BOOTARGS` replaces the default. |
| `droidvm-timebase-hz=24000000` | The host's counter runs at 1 GHz; a Mac's runs at 24 MHz, and parts of macOS 27 assume that. The kernel then scales the counter by 3/125 (1 GHz × 3 / 125 = 24 MHz). The cost is in section 13. |
| `bt-transport-override=10` | Without a Bluetooth device, `bluetoothd` waits ten seconds for a chip, exits, and is restarted, forever. This selects its built-in virtual controller, so it starts once and stays. |

Check: `cat /etc/xnulinux.conf` shows the four lines.

## 7. Build the disk on a Mac

Everything in this section runs on the **Mac**. Copy `tools/cloud/mac/` there (or clone the repository) and work in
that folder. `build-disk.sh --help` lists the stages and settings.

1. If nobody is logged in at the Mac's screen (a rented Mac), turn on automatic login and reboot. Apple's
   Virtualization framework starts a macOS guest only for a user with a desktop session. Put the Mac user's password
   in a file only you can read, then:

   ```sh
   chmod 600 ~/mac-pass
   sudo python3 autologin.py "$USER" < ~/mac-pass
   sudo reboot
   ```

   Check after the reboot: `launchctl print gui/$(id -u) >/dev/null && echo session` prints `session`.

2. Look at the Mac and set up passwordless sudo (the disk script uses `sudo -n` throughout):

   ```sh
   bash build-disk.sh preflight
   bash build-disk.sh sudoers
   ```

   `preflight` changes nothing. Expect an Apple silicon CPU, macOS 27, about 80 GB free, and `hv_vmm_present=0`.

3. Install the tools and clone the repository. Point it at this branch so the GPU bundle gets the renderer fix:

   ```sh
   REPO_URL=https://github.com/gshost1/xnulinux.git REPO_REF=cloud-arm bash build-disk.sh deps
   ```

   Check: the last lines list versions of `tart`, `zstd`, `cmake`, `ninja`, `meson`, `llvm@22`, `glslang`, and the
   repository's commit.

4. Install the guest and build the GPU bundle. Set `IPSW` to the macOS 27.2 (26B123) restore image for Apple silicon,
   as a file you downloaded from Apple or Apple's own download address for it. It must be the same build as the
   `kc27.bin` from section 4. Both stages are long; run them where they survive a dropped connection:

   ```sh
   IPSW=/path/to/UniversalMac_27.2_26B123_Restore.ipsw nohup bash build-disk.sh vm  > ~/vm.out  2>&1 &
   nohup bash build-disk.sh gpu > ~/gpu.out 2>&1 &
   ```

   Check: `tail -1 ~/xnubuild/logs/vm.log` ends in `vm stage done`, and `tail -1 ~/xnubuild/logs/gpu.log` ends in
   `gpu stage done`. For a disk with room for more software, add `VM_DISK_GB=120` to the `vm` line (the image is
   sparse; 120 GB was used for the second image).

5. Make the disk. `EXTRAS=1` gives the guest user `xnu` a password of your own and turns on Remote Login, Screen
   Sharing and your ssh key. Create the two files first (the password file on one line, mode 600; the public key is
   the one whose private half you will use from your laptop):

   ```sh
   EXTRAS=1 GUEST_PASSWORD_FILE=~/guest-pass GUEST_PUBKEY_FILE=~/guest.pub bash build-disk.sh disk
   ```

   Check: the output ends with `disk stage done` and shows three files in `~/xnubuild/macvm/`: `mac.img` (sparse; 28
   GB used here), `mac.conf` with `ROOT_UUID=`, `BOOT_UUID=` and `MACOS=27.2`, and `trustcache.bin`.

   Without `EXTRAS=1` the user is `xnu` with upstream's default password, and there is no ssh into the guest.
   Third-party applications, developer tools included, can be put on the disk by setting `APPS` to a folder of
   `.app` bundles (a feature of upstream's `mk-macvm-disk.sh`).

The image contains macOS. **Do not publish it or put it in storage others can read.**

## 8. Move the disk to the VM

The three files must end up in one folder on the VM. This guide uses `/var/tmp/macimg`.

**Simplest, when the Mac and the VM are near each other: ssh.** On the **Mac**, with a key the VM accepts:

```sh
bash build-disk.sh upload VM_USER@VM_ADDRESS /var/tmp/macimg
```

Between continents this ran at about 3 MB/s, which is hours for 28 GB.

**Faster over distance: a bucket.** Pack on the Mac, upload the parts in parallel, compose them, pull on the VM.
The scripts are written for Google Cloud Storage and need `gcloud` logged in on your laptop; set `BUCKET`, `MAC_HOST`,
`MAC_USER` and `MAC_KEY` in `cloud.env`. Any other object store works the same way with its own commands: upload
`part.*`, then on the VM `cat` them in name order into `zstd -d | tar -xSf -`.

1. **Mac.**

   ```sh
   bash pack-image.sh ~/xnubuild/macvm img1
   ```

   Expected: `packed in NN s: NN parts, NNG`.

2. **Laptop.** Send the parts and check every one:

   ```sh
   tools/cloud/relay-mac-to-bucket.sh img1
   ```

   Expected last line: `relay: OK gs://BUCKET/img1.tar.zst NNN bytes from NN verified parts`. The Mac never gets cloud
   credentials: the laptop makes one upload address per part. A rerun sends only parts that are missing or wrong.

3. **Laptop.** Pull on the VM. The access token travels on standard input:

   ```sh
   gcloud auth print-access-token | tools/cloud/vm-run tools/cloud/vm/pull-image.sh "$BUCKET" img1 /var/tmp/macimg
   ```

   Expected: `pulled in NNN s`, then the three files and the contents of `mac.conf`.

Check on the **VM**, whichever way you used:

```sh
ls -ls /var/tmp/macimg/        # mac.img, mac.conf, trustcache.bin
cat /var/tmp/macimg/mac.conf   # ROOT_UUID=... BOOT_UUID=... MACOS=27.2
```

## 9. Boot and stop

1. **Laptop.** Boot the image and wait 100 seconds:

   ```sh
   tools/cloud/vm-run tools/cloud/vm/boot-image.sh /var/tmp/macimg 100
   ```

   It copies the trust cache and `mac.conf` into place, turns off "panic on oops" (so a fault in the hosted kernel
   ends the run instead of rebooting the VM), and runs `scripts/xnulinux-boot.sh`. Expected: the end of the hosted
   console, with lines from `launchd` and a prompt `xnulinux#`, and `xnu.ko loaded: 1`.

2. **Laptop.** Look at the screen about a minute later:

   ```sh
   tools/cloud/guest-screenshot shot.png
   ```

   Expected: a 1280×800 PNG of the desktop (wallpaper, menu bar, Dock).

3. To stop cleanly, sync inside the guest, then stop from the **VM**:

   ```sh
   tools/cloud/guest-ssh 'sync; sync'
   tools/cloud/vm-ssh 'cd ~/w/poc && bash scripts/xnulinux-boot.sh stop'
   ```

   Expected: `hosted system stopped`. Stop it before you stop or delete the VM.

To boot again after a VM restart, run step 1 again. To have it start with the VM, see `scripts/install-service.sh`
(not tried on this host).

## 10. Reach the hosted system

Set `GUEST_KEY` and `GUEST_PASSWORD_FILE` in `tools/cloud/cloud.env` on your laptop. The guest gets its address from
the VM by DHCP; the tools look it up.

- **A root shell that always works: the console.** It needs no login and no network in the guest.

  ```sh
  tools/cloud/guest-console 'sw_vers' 8
  ```

  Expected: `ProductName: macOS`, `ProductVersion: 27.2`, `BuildVersion: 26B123`. Keep lines short; the console is slow.

- **ssh** (needs `EXTRAS=1` in section 7). It jumps through the VM:

  ```sh
  tools/cloud/guest-ssh 'sw_vers -productVersion; whoami'      # expect: 27.2 and xnu
  tools/cloud/guest-ssh 'sudo -S -p "" id -u' < "$GUEST_PASSWORD_FILE"   # expect: 0
  ```

  The second form is how to become root: the password goes over standard input, never on a command line.

- **Screenshots.** `tools/cloud/guest-screenshot out.png`. The dump lags the screen by a few seconds.

- **Keyboard and pointer.**

  ```sh
  tools/cloud/guest-input click 640 400
  tools/cloud/guest-input key cmd space
  tools/cloud/guest-input type 'hello' key return
  ```

  Coordinates are pixels on the 1280×800 screen. There is no acknowledgement: take a screenshot to see the effect.

- **Logging in at a login or lock window.** The disk logs in automatically at boot. But if the window server crashes,
  the session ends at a login window, and there is nobody to type. Do it from the laptop:

  1. `tools/cloud/guest-screenshot now.png`, and look at it. You must see the password field.
  2. If the field is not focused, click it (for the lock screen at this resolution: `tools/cloud/guest-input click 640 718`).
  3. `tools/cloud/guest-login`

  Never skip the fresh screenshot. A password typed while the screen is still changing goes to whatever has the
  keyboard at that moment.

- **Screen Sharing.** Open a tunnel and connect with the Screen Sharing app on a Mac:

  ```sh
  tools/cloud/guest-ssh -L 5900:localhost:5900 -N
  ```

  Then connect to `vnc://localhost:5900` as `xnu`. The server offers only Apple's authentication types, so a generic
  VNC client will not connect. Only the protocol handshake was checked here, not a full session.

## 11. Tuning for CPU rendering

With `GPU=off` the window server alone takes about one CPU while the screen is changing. These changes were applied
inside the guest and kept. They can all be undone. **The gain was not measured**: no before-figure was taken.

**Guest**, as root (`sudo -S` over `guest-ssh`, or the console):

```sh
for l in com.apple.security.cryptexd com.apple.locationd com.apple.icloud.searchpartyd com.apple.findmy.findmybeaconingd \
  com.apple.icloud.findmydeviced com.apple.findmymacd com.apple.findmymacmessenger com.apple.softwareupdated \
  com.apple.mobile.softwareupdated com.apple.adid com.apple.fairplayd com.apple.fairplaydeviceidentityd \
  com.apple.modelcatalogd com.apple.perfpowermetricd; do launchctl disable system/$l; done
mdutil -a -i off
pmset -a sleep 0 displaysleep 0 disksleep 0 powernap 0
```

`cryptexd` and `locationd` are on the list because they crashed repeatedly here; the others are idle work for hardware
or services the hosted system does not have.

**Guest**, as the user `xnu` (less for the compositor to blend and animate):

```sh
defaults write com.apple.universalaccess reduceTransparency -bool true
defaults write com.apple.universalaccess reduceMotion -bool true
defaults write com.apple.Accessibility ReduceMotionEnabled -bool true
```

These three lines are the settings that were applied, but the exact command lines were not kept; check them in System
Settings, Accessibility, Display if they do not take.

Then reboot the hosted system (section 9). Check: `launchctl print-disabled system | grep -c 'disabled'` is 14 or
more, and `top -l 2 | grep 'CPU usage' | tail -1` shows about 90 % idle a few minutes after login (the first sample of `top` is not meaningful).

Two cautions from experience:

- Some user agents for the voice assistant, suggestions, weather and tips were also disabled here. **Leave
  `com.apple.siriactionsd` enabled.** Disabling it makes three control-centre widgets crash at every login
  (Troubleshooting, "widgets crash at login").
- `softwareupdated` kept running although disabled.

## 12. Check the result

Run these from the **laptop** and compare:

```sh
tools/cloud/vm-ssh 'lsmod | grep -c "^xnu "'                                  # 1
tools/cloud/guest-ssh 'sw_vers -productVersion; pgrep -x Dock >/dev/null && echo desktop-up'   # 27.2, desktop-up
tools/cloud/guest-ssh 'launchctl print system/com.apple.bluetoothd | grep -E "runs|last exit"' # runs = 1, never exited
tools/cloud/guest-screenshot check.png                                        # a PNG of the desktop
tools/cloud/boot-cycle.sh 0 check                                             # dock=up ... crash-reports: ...
```

The pointer-authentication fix (needs `clang` in the guest):

```sh
tar -C tests -cf - hosted | tools/cloud/guest-ssh 'rm -rf ~/xt && mkdir ~/xt && tar -C ~/xt -xf - && sh ~/xt/hosted/run.sh'
```

Expected last line: `hosted tests: pass`. Details: [`tests/hosted/README.md`](../tests/hosted/README.md).

The renderer fix has its own test, a plain Vulkan program that is built on the Mac against the lavapipe from the GPU
bundle build: [`tests/lavapipe/README.md`](../tests/lavapipe/README.md). Expected last line: `RESULT: PASS`.

## 13. Measured numbers

All on the C4A VM described in section 1, hosted macOS 27.2 with 3 CPUs, CPU rendering.

| What | Value |
|---|---|
| Idle, a few minutes after login, after tuning | 90 to 96 % of 3 CPUs idle, about 470 processes |
| Window server while the screen changes | about 97 % of one CPU |
| Desktop after the root shell | about 1 minute; Dock and Finder 25 s after the window server |
| First C compile in the guest | 15 s |
| A new Swift package, `swift build` | 18 s |
| Kernel rebuild after a patch | 26 s, plus 9 s for the collection |
| Icon-service renderer crashes in the first 240 s after login | 17 and 19 in two boots before the renderer fix; 0, 0, 0 in three boots after |

**Cost of the 24 MHz timebase.** With the counter scaled, a user program cannot read the hardware counter directly;
every read is a trap into the kernel. Measured in the guest:

| Call | Time |
|---|---|
| `mach_absolute_time` | 400 ns |
| `mach_continuous_time` | 400 ns |
| `clock_gettime` | 404 ns |
| `getppid` (a plain system call, for comparison) | 407 ns |

So a timebase read costs the same as a system call. With `xnu.ko perf=1`, the first 8 minutes of a boot had 51.0
million traps from user space of all kinds, and idle had 1,500 to 3,000 a second. Even if every one were a timebase
read, that is 51 M × 0.4 µs = about 20 s of CPU out of about 1,440 CPU-seconds: **at most 1.4 %**, and about 0.1 % of
one CPU at idle. A faster path for user space was judged not worth building.

**Pointer-authentication keys**, from `tests/hosted/keys.c`:

| Process | Real Apple silicon Mac | Hosted, before the fix | Hosted, with the fix |
|---|---|---|---|
| plain `arm64` | all four off | IA off, IB **on**, DA off, DB **on** | all four off |
| `arm64e` | all four on | all four on | all four on |

## 14. Troubleshooting

Each entry: what you see, why, what to do. All of these happened.

### Host and build

**`cma: Failed to reserve 8192 MiB` at boot, after adding `cma=8G`.**
Cause: on arm64 the global CMA area must lie below 4 GB. A per-node area (`numa_cma=0:8G`) does reserve the memory,
but `fki.ko` does not use it.
Fix: do not use CMA. Use `mem=` and the carve-out (section 5).

**`apt-get` fails on `p7zip-full` ("has no installation candidate").**
Cause: Ubuntu 26.04 dropped the package; `7zip` provides `7z`.
Fix: fixed in this branch (`scripts/vm-xnu-build.sh` asks for it only where apt has it).

**The build stops with `sed: can't read toolchains/linux/cctools-work/.../thread_status.h: No such file or directory`.**
Cause: the script patched a toolchain tree that is only cloned later, so it failed on a host that had never built.
Fix: fixed in this branch.

**`ConnectionResetError: [Errno 104] Connection reset by peer` during `setup-appleclang.py fetch`.**
Cause: the network.
Fix: run the script again. Downloads are checksummed, and what is complete is not fetched twice.

**The build stops making progress; `decomment` processes use a full CPU each; the disk fills.**
Cause: a helper in XNU's build keeps `getc()`'s result in a `char`. On arm64 Linux `char` is unsigned, the end-of-file
value never matches, and it loops forever, writing `*.strip.<pid>` files of about 8 GB each. The build script has the
fix (`char` to `int`), but it can only apply it once the source tree exists, and on a fresh host the tree is created
by the same step that then runs the helper. So the first pass always hangs.
Fix: section 4 step 2: kill, delete the temporary files, run the script again. The second pass applies the fix first.
Not fixed in this branch: a clean fix belongs inside macOS-DroidVM's `setup-env.py`.

**`System ERROR: errno=28 : No space left on device` while extracting the Kernel Debug Kit.**
Cause: the temporary files from the hang above.
Fix: delete `*.strip.*` and `*.unifdef.*` under `~/w/dvm/out`, then run again.

**`cp: cannot create regular file '/lib/firmware/xnu/kernel.macho'`.**
Cause: nothing created the directory on a fresh host.
Fix: fixed in this branch (`scripts/vm-xnu-rebuild.sh` creates it).

**The kernel traps at once: `Oops - BRK`, with `wfe_timeout_configure` and `___ubsan_handle_sub_overflow_minimal`
when symbolised. Seen when loading a kernel built without `kc27.bin`.**
Cause: without `kc27.bin` the rebuild script skips the step that lays out the kernel's segments, and links with a
stale layout from macOS-DroidVM. xnulinux's patches make the code segment bigger, so it overlaps the next segment;
the linker says nothing. At run time a string the kernel needs has been overwritten, the device-tree lookup for the
clock fails, the timebase reads as zero, and an overflow check traps. It has nothing to do with the host.
Fix: always put `kc27.bin` in place before `vm-xnu-rebuild.sh` (section 4). The module's bare-kernel modes (`mode=1`,
`mode=2`) are bring-up leftovers; `mode=2` also fails later for another reason. Only `mode=3`, which the boot script
uses, is expected to work.

**After a crash of the VM, `insmod: ERROR: could not insert module fki-ko/fki.ko: Invalid parameters`.**
Cause: the VM rebooted on a kernel panic before the freshly built modules were written to disk; they are empty files.
Fix: rebuild the modules (section 4 step 6), and `sync` before any risky load. `boot-image.sh` turns off
`panic_on_oops` so that a fault in the hosted kernel no longer reboots the VM.

**The hosted system stops at `Waiting on ... IOMedia ... Apple_HFS`.**
Cause: no disk, or the identifiers in `mac.conf` are not this disk's.
Fix: check that `/var/tmp/mac.conf` is the one that came with `mac.img`.

**Programs are killed at launch on a 27.2 disk; the log talks about code signatures and platform.**
Cause: the collection was patched for the wrong platform version. The script's default is 26 (for a 27.0.1 disk).
Fix: `AMFI_PLATFORM=27 bash scripts/vm-xnu-rebuild.sh`.

### Mac

**`tart create` ends with "The virtual machine failed to start".**
Cause: the user has no desktop session (nobody logged in at the screen).
Fix: automatic login and a reboot (section 7 step 1).

**`brew install cirruslabs/cli/tart` no longer works.**
Cause: tart moved to another Homebrew tap, and Homebrew ignores a third-party tap until it is trusted.
Fix: `build-disk.sh deps` does `brew tap openai/tools; brew trust openai/tools; brew install openai/tools/tart`.

**Mesa's build stops with a Python error.**
Cause: Mesa needs Python 3.10 or newer; the system `python3` is 3.9.
Fix: `build-disk.sh gpu` makes its virtual environment from Homebrew's Python 3.13.

**The desktop never appears; the hosted log says
`XnuCpuMetal: dlopen(libvulkan_lvp.dylib) failed: symbol not found in flat namespace '_spvBinaryToText'`
and the window server aborts in `__WSSystemCanCompositeWithMetal`.**
Cause: Homebrew's `spirv-tools` is installed (it comes with `glslang`). Mesa finds it through pkg-config and compiles
calls to it into lavapipe, but the driver is linked without that library.
Fix: `brew unlink spirv-tools`, delete the lavapipe build folders, rebuild lavapipe and the bundle. `build-disk.sh
gpu` now does this and refuses a bundle with undefined `_spv` symbols. Upstream has a similar change on a CI branch.

### Transfer

**`zstd: Data corruption detected` when unpacking on the VM.**
Cause: one part failed to upload in a second push, and the stale part from the first push went into the composed
object.
Fix: compare checksums of every part with the source before composing. `relay-mac-to-bucket.sh` does: it refuses to
compose until all match, and stops if the bucket holds a part the Mac does not have.

### In the hosted system

**Notification and Dock icons are blank squares; `iconservicesagent` crashes again and again
(`EXC_BAD_ACCESS` in `lp_rast_triangle_32_*` inside `libvulkan_lvp.dylib`); once the window server died the same way
and the session ended at a login window.**
Cause: a bug in Mesa's lavapipe. When a descriptor set is pushed over one with another layout, lavapipe copies the
old set's bytes over the new one and loses its built-in ("immutable") sampler. The shader then indexes a table with
garbage. It is intermittent: four of six boots had it.
Fix: in this branch, `gpu/xnu-cpu-metal/patches/mesa/0002-lavapipe-push-immutable-samplers.patch`
([upstream PR #17](https://github.com/AFK-surf/xnulinux/pull/17)). Rebuild the GPU bundle from this branch, or swap
the library in a running system with `tools/cloud/install-renderer-lib.sh`. Not reported to Mesa.

**Applications that are plain `arm64` (not `arm64e`) crash when a window or sheet opens, or at the first exception:
`EXC_ARM_PAC_FAIL` or SIGBUS in `libunwind`.** This includes most third-party software and some developer tools.
Cause: the hosted kernel left two of the four pointer-authentication keys (IB and DB) switched on for plain `arm64`
processes. The system unwinder checks each return address with the IB key; a plain `arm64` frame's return address is
unsigned; on a CPU that faults on a failed check (FEAT_FPAC, as Neoverse V2 has) the process dies. A real Mac runs
such processes with all four keys off.
Fix: in this branch, in `xnu-patches/0001-linux64-machine-layer.patch`
([upstream PR #15](https://github.com/AFK-surf/xnulinux/pull/15)). Rebuild the kernel (section 4 step 5) and reboot
the hosted system. UniXNU fixed the same thing in its kernel earlier.

**`bluetoothd` starts every ten seconds; the log fills with `AppleVPKeyStore ... unexpected session uid: -1 requested
by: bluetoothd`.**
Cause: with no Bluetooth device described, it assumes a real Mac's chip, waits ten seconds for its driver, gives up
and exits; launchd restarts it. The keystore lines are a harmless query at each start, not the cause.
Fix: boot argument `bt-transport-override=10` (section 6).
[Upstream issue #16](https://github.com/AFK-surf/xnulinux/issues/16) proposes describing the device instead.

**`ScreenshotControls`, `DisplayControls` and `WindowManagerControlsExtension` crash at every login with a Swift
assertion in WidgetKit.**
Cause: self-inflicted. The tuning had disabled `com.apple.siriactionsd`, which those widgets ask for their actions.
The same macOS build in a normal virtual Mac does not crash.
Fix: leave `com.apple.siriactionsd` enabled (`launchctl enable gui/501/com.apple.siriactionsd`, then log in again).
That the crashes stop was not verified afterwards.

**`cryptexd` crashes in a loop; `locationd` crashes at boot.**
Cause: not investigated. Both depend on things the hosted system does not have.
Fix: disable them (section 11).

**The session is at a login window and nothing can type.**
Fix: section 10, "Logging in at a login or lock window".

**A permission prompt for "sshd-keygen-wrapper" appears.**
Cause: something run over ssh used automation (for example `osascript` talking to System Events).
Fix: answer it with `guest-input`, or avoid automation over ssh.

**`log show` shows nothing from your own program or library.**
Cause: unknown. Do not rely on it for your own messages; write to a file.

**`sysctl hw.optional.arm.FEAT_PAuth` says 0.**
Pointer authentication works all the same. Not investigated.

**The hosted kernel log says `do_cacheid() - 512 bytes Unknown cache`, `failed to find the interrupt-controller`,
`failed to find cpu-debug-interface`.**
Harmless. The first is XNU decoding this CPU's cache registers wrongly (the line size is right). The others are by
design: Linux owns the interrupt controller.

## 15. What does not work, and what was not tested

Does not work or is missing:

- **Graphics speed.** Everything is drawn on the CPU. The desktop is usable, not fast. Video and games were not tried.
- **Sound, camera, Bluetooth devices, USB.** None exist.
- **Apple account services** (sign-in, the App Store, iCloud). Not tried. macOS-DroidVM reports that sign-in fails on
  its hosts; expect the same and plan on direct downloads.
- **Security.** Code-signing enforcement and system integrity protection are relaxed by the boot arguments, and the
  system volume's seal is broken by upstream's disk script. Do not put anything you care about in it.
- **`softwareupdated`** still runs though disabled. Do not install system updates: the kernel collection is tied to
  one build.
- **A Spotlight indexing extension of the voice-memo app** crashes; `AppleKeyStoreUserClient failed MACF` messages from
  three daemons continue. Not investigated.
- **A chess game crashed twice** (`EXC_ARM_DA_ALIGN` in a texture upload) before the two fixes and not in three tries
  after. The cause was never established.

Not tested:

- **This exact branch, end to end.** The system described here was built from upstream at commit `06bc271` plus local
  patches. The two fixes are the same changes in upstream's form, the fixed renderer was rebuilt from that form, and
  the build-script fixes were applied by hand on the VM in an earlier shape. A fresh VM built from `cloud-arm` from
  the first step has not been done.
- **The helper tools in their cleaned form.** Each tool's header says whether it was run or only syntax-checked. Most
  were only syntax-checked; their private originals did the work.
- **Restoring a saved image on a new VM** (section 16).
- **More than 3 hosted CPUs**, other VM sizes, other clouds, other CPUs, a 16 KB-page host kernel.
- **Running as a service** (`scripts/install-service.sh`) on this host.
- **A full Screen Sharing session**; only the handshake.
- **Long uptimes and load.** The longest runs were hours, not days. No stress run like upstream's was done here.
- **Which of the three parts of the renderer patch** is needed. The crash dump points at the first.

## 16. Saving your work

Once the hosted system is set up, save the image and the kernel side, so a deleted VM is not a lost afternoon.

1. Stop the hosted system (section 9).
2. **Laptop.**

   ```sh
   tools/cloud/vm-run tools/cloud/vm/save-image.sh /var/tmp/macimg save1
   tools/cloud/relay-vm-to-bucket.sh save1
   tools/cloud/relay-vm-to-bucket.sh save1k
   ```

   Expected: `READBACK-OK ...`, then for each relay `relay: OK ... all NN parts match the VM's MD5`. Here a 29 GB
   image packed in 226 s and uploaded in 148 s.
3. Boot again (section 9), and delete the staging folder `/var/tmp/up` on the VM when the upload is verified.

`save1k` holds the built kernel collection, the modules (built for the host kernel that was running), `kc27.bin`, the
settings file and the kernel command line. Both archives contain Apple's files: private storage only. Restoring them
on a fresh VM was not tested.

## Credits

This is a small addition to other people's work: [xnulinux](https://github.com/AFK-surf/xnulinux) (the whole
approach of hosting XNU inside Linux, and every script this guide drives),
[macOS-DroidVM](https://github.com/Droid-VM/macOS-DroidVM) (the 4 KB-page XNU base, the Linux toolchain, the timebase
scaling option), [UniXNU](https://github.com/UniXNU/UniXNU) (which fixed the pointer-authentication keys first, and whose
notes pointed at several of the tuning steps), and [metal2vulkan](https://github.com/steelbrain/metal2vulkan) with
[Mesa](https://mesa3d.org)'s lavapipe, which draw the desktop.

macOS is a trademark of Apple Inc. This work is not affiliated with or endorsed by Apple.
