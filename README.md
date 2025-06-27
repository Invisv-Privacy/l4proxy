# INVISV L4Proxy

## What is INVISV L4Proxy?
INVISV **L4Proxy** is a high-performance first-hop MASQUE proxy Network Function Virtualization (NFV) module. INVISV **L4Proxy** provides server-side functionality needed for running a [Multi-Party Relay](https://invisv.com/articles/relay.html) service to protect users' network privacy.

## Installation
INVISV **L4Proxy** runs in BESS, the Berkeley Extensible Software Switch. A BESS installation is required. Code in the l4proxy directory must be merged into the base BESS directory:
   ```bash
   cp -R l4proxy/* ${bess_dir}/
   ```
