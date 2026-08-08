# Capability and Feature Analysis: newos Userland

Based on an assessment of the current `src/tools` directory against the needs of a standalone, Linux-ABI-compatible operating system environment, the following analysis identifies missing capabilities and system utilities.

*(Note: Per requirements, gaps related to User/Group management have been intentionally excluded from this analysis).*

## Missing Capabilities Overview

| Category | Missing Capability / Feature | Suggested Tools | Priority / Impact |
| :--- | :--- | :--- | :--- |
| **Storage & Filesystems** | Block device discovery, partitioning, and filesystem creation/checking. | `lsblk`, `blkid`, `fdisk` / `parted`, `mkfs.*`, `fsck` | **High** |
| **System Scheduling** | Scheduled, recurring, or background job automation. | `cron` / `crond`, `at`, `nohup` | **High** |
| **Kernel & Hardware** | PCI/hardware enumeration and dynamic kernel module management. | `lspci`, `lscpu`, `lsmod`, `modprobe`, `rmmod` | **High** |
| **Network Security** | Packet filtering, firewall configuration, and NAT manipulation. | `iptables`, `nft`, `ufw` | **Medium** |
| **Backup & Sync** | Efficient, delta-encoded file synchronization over networks. | `rsync` | **Medium** |
| **Package Management**| Retrieving and managing external software beyond the base system. | `apk`, `pacman`, or a custom `pkg` tool | **Medium** |
| **Terminal Multiplexing**| Persistent remote shell sessions and window management. | `tmux`, `screen` | **Low** |
| **Wireless Networking** | Configuration and authentication for Wi-Fi hardware. | `iw`, `wpa_supplicant` | **Low** |

---

## Elaborations

### 1. Storage & Filesystems (High Priority)
While the system has `mount`, `umount`, `df`, `du`, and `dd`, it completely lacks the tools necessary to provision a bare-metal disk. A fully capable userland needs to format drives, modify partition tables, and perform filesystem consistency checks.
*   **Missing Features:** Partition editing, block device UUID identification, filesystem creation, and maintenance.
*   **Recommendation:** Implement a basic partition editor (`fdisk`), block device listing (`lsblk` and `blkid`), and foundational filesystem tools (`mkfs.ext4`, `fsck`).

### 2. System Scheduling (High Priority)
A standard operating system environment requires the ability to execute tasks asynchronously or on a fixed schedule (e.g., log rotation, backups, system maintenance). The current toolset focuses entirely on foreground/interactive or service-supervised execution.
*   **Missing Features:** Cron daemon, user-level crontabs, and deferred execution (`at`).
*   **Recommendation:** Implement a lightweight `crond` service and the `crontab` utility to parse standard cron expressions.

### 3. Kernel & Hardware Management (High Priority)
The OS includes `lsusb` and `usbmon` for USB devices, but lacks visibility into the PCI bus or CPU topology. Furthermore, assuming Linux ABI compatibility, users often need to load or unload kernel drivers dynamically.
*   **Missing Features:** PCI device enumeration, module loading/unloading, and hardware summary.
*   **Recommendation:** Implement `lspci` for hardware discovery and `lsmod`/`modprobe` to interface with the Linux kernel's module management (`init_module`/`delete_module` syscalls).

### 4. Network Security & Firewall (Medium Priority)
The environment boasts an impressive array of networking tools (`httpd`, `netcat`, `ssh`, `ip`, `ss`, `dhcp`), but does not have userland tools for securing network perimeters or establishing routing NAT.
*   **Missing Features:** Interfacing with Linux Netfilter for port forwarding, blocking IPs, and stateful packet inspection.
*   **Recommendation:** Introduce a simplified frontend for `nftables` (e.g., `nft`) or `iptables` to give administrators control over inbound and outbound traffic.

### 5. Backup & Synchronization (Medium Priority)
For data movement, the system currently provides `cp` and `scp`. However, these are inefficient for large directory trees or incremental backups, which are common administrative tasks.
*   **Missing Features:** Delta-transfer algorithms for syncing files.
*   **Recommendation:** A lightweight implementation of `rsync` or a compatible delta-sync tool would vastly improve data management across nodes.

### 6. Package Management (Medium Priority)
Currently, `newos` functions as a single unified monolithic tree (built via `make`). If the system is to be adopted for daily driver or server usage, users will need to install dependencies outside the baseline repository.
*   **Missing Features:** Fetching, installing, upgrading, and resolving dependencies for third-party binaries or libraries.
*   **Recommendation:** Add a simple package manager (`pkg`) capable of extracting `.tar.gz` or `.apk` archives into root, tracking manifests.

### 7. Terminal Multiplexing (Low Priority)
When users SSH into the system (using the provided `sshd`), their sessions will die if the connection drops. Multiplexers are a standard expectation for remote UNIX work.
*   **Missing Features:** Session detachment, window splitting.
*   **Recommendation:** A simplified `tmux` clone or `screen` capability would significantly improve remote usability.

### 8. Wireless Networking (Low Priority)
The `ip` tool is excellent for wired interfaces and routing, but modern standalone devices (laptops, IoT) frequently rely on wireless networking which requires specific cryptographic handshakes.
*   **Missing Features:** WPA2/WPA3 authentication and wireless interface configuration.
*   **Recommendation:** Implement or port a lightweight `wpa_supplicant` and `iw` for scanning and connecting to Wi-Fi networks.
