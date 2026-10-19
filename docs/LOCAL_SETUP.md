# Moving sohamalloc work to the ROG Zephyrus G16

This guide sets up the laptop (Core Ultra 9, 32 GB, RTX 4080) so the project continues there. Benchmarks then run on real hardware, with `perf` and hardware counters, and nothing is lost when a cloud container is reclaimed.

**What runs where.** This laptop model's Core Ultra 9 is very likely the 185H: 6 performance cores, 8 efficiency cores and 2 low-power cores, 22 threads. The setup script prints the exact topology. The GPU is not used: sohamalloc is a CPU memory allocator.

## Choose a path

| Path | Time | Good for | Not good for |
|---|---|---|---|
| **A. WSL2** (Ubuntu inside Windows) | 20–30 min, no risk to Windows | Development, tests, code review, the research log | Paper-quality timings: it is a virtual machine, and `perf` mostly doesn't work |
| **B. Native Ubuntu 24.04** | 1–2 h | Everything, including the benchmark numbers for the paper | — |

You can start with A today and add B later. The steps from **part 4** on are the same for both.

For path B there are three safe ways to place Linux:

1. **A second internal SSD, if the laptop has one.** "3 TB" often means 1 TB + 2 TB. Check Windows: Win+X → Disk Management. If a whole disk is empty or holds only data you have backed up, install Linux there. Windows is never touched.
2. **An external USB-C SSD.** Zero risk to Windows. Pick it from the boot menu when you want Linux. Benchmarks are CPU-bound, so USB speed doesn't matter.
3. **Dual boot on the main SSD.** Shrink Windows and install next to it. Standard, but read part 1 carefully.

---

## Path A: WSL2 (quick start)

1. Open **PowerShell as Administrator** (Start → type "PowerShell" → right-click → Run as administrator).
2. Run `wsl --install -d Ubuntu-24.04`, then reboot when asked.
3. Ubuntu opens and asks for a new Linux user name and password (they are separate from Windows).
4. In the Ubuntu window, continue with **part 4**.

## Path B: native Ubuntu 24.04 LTS

### Part 1: Prepare Windows

1. **Back up** anything important.
2. **Save the BitLocker recovery key.**
   - Windows 11 on ASUS laptops often has device encryption on. Changing boot settings can make Windows ask for the key once.
   - Find it at <https://aka.ms/myrecoverykey>, or Settings → Privacy & security → Device encryption.
   - Photograph it or print it.
3. **Turn off Fast Startup.**
   - Control Panel → Power Options → "Choose what the power buttons do" → "Change settings that are currently unavailable".
   - Untick "Turn on fast startup". This keeps Windows from locking the disk.
4. **Dual boot on the main SSD only: make room.**
   - Win+X → Disk Management → right-click **C:** → Shrink Volume.
   - Shrink by 250 GB or more (enter 256000 MB). Leave the space "Unallocated".
5. **Make the installer USB stick** (8 GB or more; it will be erased).
   - Download the latest **Ubuntu 24.04.x LTS Desktop** ISO from <https://ubuntu.com/download/desktop>. Later point releases have newer kernels, which this 2024 hardware needs.
   - Write it with **Rufus** (<https://rufus.ie>): Partition scheme **GPT**, Target system **UEFI**.

### Part 2: Install

1. Plug in the USB stick and the charger, then restart.
2. Open the **boot menu**: tap **Esc** (or **F8**) right after power-on, and choose the USB stick (the "UEFI: …" entry).
   - For the BIOS itself, tap **F2**.
   - Secure Boot can stay on: Ubuntu supports it. If the stick will not boot, turn Secure Boot off in the BIOS, then back on after installing.
3. Choose "Try or Install Ubuntu" → **Install Ubuntu**.
4. Pick language and keyboard, connect to Wi-Fi, and choose **Interactive installation** with the default app selection.
5. Tick **"Install third-party software for graphics and Wi-Fi hardware"**.
6. **Disk setup.** Read this screen twice: the wrong choice erases Windows.
   - **Dual boot:** choose **"Install Ubuntu alongside Windows Boot Manager"** and give it the unallocated space.
   - **Second SSD or external SSD:** choose **Manual installation**.
     - Select the *correct* disk (check its size), and create:
       - a 1 GB FAT32 partition mounted at `/boot/efi`;
       - an ext4 partition mounted at `/` using the rest of the disk.
     - Set **"Device for boot loader installation"** to that same disk.
   - **Never** choose "Erase disk and install Ubuntu" unless the selected disk is the empty one.
   - **If no disk appears at all,** the BIOS storage mode is Intel VMD/RST. Stop here and ask before changing it: switching it off without preparation makes Windows unbootable.
7. Set time zone (Chicago), user name and password, then wait for the install.
8. Reboot and remove the USB stick when told. A menu (GRUB) now lets you pick Ubuntu or Windows at each start. If Windows asks for the BitLocker key once, enter it.

### Part 3: First boot

1. Open a terminal (Ctrl+Alt+T) and update:

   ```sh
   sudo apt update && sudo apt full-upgrade -y && sudo reboot
   ```
2. **NVIDIA driver** (optional for this project): run `sudo ubuntu-drivers install`, then reboot.
   - With Secure Boot on, the installer asks you to set a password.
   - At the next boot a blue screen appears: choose **Enroll MOK** → Continue → enter that password.
3. **Known quirk:** the built-in speakers may be quiet or silent on Linux, a kernel issue with this model's amplifiers. Headphones and Bluetooth work. It does not affect this project.
4. Settings → Power: choose **Performance**, and set "Automatic suspend" to **Off** while plugged in, so long runs are not interrupted.

---

## Part 4: Get the project and the tools (paths A and B)

```sh
sudo apt install -y git curl
git clone https://github.com/SohamBasanwar/sohamalloc.git ~/sohamalloc
cd ~/sohamalloc
git checkout sohamalloc-dev
git config user.name  "Soham Basanwar"
git config user.email "sohambasanwar@gmail.com"
tools/setup-workstation.sh
```

`setup-workstation.sh` takes about 15–30 minutes. It:

- installs about 40 packages: compilers, the allocators we compare against, `perf`, the cross-platform toolchains, and `redis` and `sqlite`;
- builds sohamalloc;
- runs the quick test suite;
- downloads and builds the benchmark suite;
- writes a record of the machine to `docs/research-log/data/` (needed for the paper's methods section).

## Part 5: Install Claude Code and sign in

```sh
curl -fsSL https://claude.ai/install.sh | bash
```

Open a **new** terminal, then:

```sh
claude --version        # prints a version number
cd ~/sohamalloc
claude                  # first run: a browser opens; sign in with the same claude.ai account
```

Inside Claude Code, type `/exit` to leave again.

## Part 6: Continue this exact session on the laptop

From the project folder, with no uncommitted changes:

```sh
cd ~/sohamalloc
claude --teleport session_012rXeiiXRUZXX5rXeCbp7ee
```

This loads the whole conversation history and checks out branch `sohamalloc-dev`.

**If teleport fails** (for example a sign-in error), run `claude` in the folder instead and paste the prompt from `docs/HANDOFF.md`. All the context is in the repository.

**To steer it from your phone or claude.ai,** type `/remote-control` (or `/rc`) inside the local session. It then appears in the Claude app.

- The laptop has to stay on, awake and online.
- Nothing opens an inbound port: the connection is outbound HTTPS only.

## Part 7: Before every benchmark campaign (native Ubuntu)

Benchmarks need a quiet, stable machine:

- plugged in;
- lid open;
- no browser or downloads running.

```sh
tools/bench-host.sh status          # shows governor, turbo, perf access, P-core and E-core CPU lists
sudo tools/bench-host.sh tune       # performance governor, turbo off, perf counters readable
# ... run the campaign ...
sudo tools/bench-host.sh restore    # back to normal
```

Turbo off makes laptop numbers repeatable. Its state is recorded and reported in the paper.

## Data use

Nothing on the laptop is sent anywhere except through Claude Code itself, which sends the files and command output it works with to Anthropic's API. Do not put unrelated private files in `~/sohamalloc`.
