# Solarflare Kernel-Bypass with CAPIO

Memory-safe userspace networking architecture for a Solarflare E10 NIC on ARM Morello. Extends the existing CAPIO (capability-I/O framework) framework originally designed for the Intel e1000e, a NIC not originally meant for kernel-bypass networking.

Built as a quarter-long project for [COMP_SCI 446: Kernel and other Low-level Software Development](http://pdinda.org/lowlevel/) at Northwestern University.

[Read the paper here](https://drive.google.com/file/d/1CAhhlSaFIl4PQ_LeZ-I6vQf6aBsJuYef/view?usp=sharing) 

## Motivation 

Kernel-bypass networking cuts latency by mapping NIC doorbell registers and DMA buffers directly into a userspace process, removing per-packet system calls and the kernel network stack. However, an unprivleged process having this level of access comes with obvious safety concerns, and the MMU can only isolate memory at a 4KB page granularity. This project uses CHERI capabilities to collapse that isolation down to the individual register and buffer, with every pointer becoming a 128-bit capability whose bounds and permissions are enforced in the host's hardware, so an out-of-bounds access traps rather than silently corrupting device state. We hope that this serves as a proof of concept that host-based capability hardware can deliver the same memory protection as proprietary NIC silicon (like Solarflare's Virtual Interfaces), a step toward high-performance networking secured by general-purpose host hardware rather than vendor-specific features.

## Architecture 

The driver is split across a control plane (kernel) and a data plane (userspace)

- **Kernel stub (control plane):** handles PCIe bring-up, MCDI firmware init, DMA-coherent allocation, and interrupt-driven control events, then mints narrowly-bounded CHERI capabilities over the doorbell registers and DMA rings and hands them to userspace.
- **Userspace driver (data plane):** an ef_vi-style driver that posts descriptors, rings MMIO doorbells, and busy-polls the event queue — never entering the kernel on the per-packet hot path.
- **Two event queues:** data events feed a userspace-pollable queue while rarer control events stay in the kernel via interrupts, keeping system calls off the hot path.

## Environment

- ARM Morello
- CheriBSD
- Solarflare SFN7322F-R2 10GbE NIC

## Authors 

Finn McMillan and Arthur Zakrzewski -- Northwestern University

With thanks to Friedrich Doku, Josh Brice, and Prof. Peter Dinda and the NU Prescience Lab!

