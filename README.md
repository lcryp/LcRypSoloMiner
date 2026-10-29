# LcRyp Solo Miner v2.2

Solo miner for the **LcRyp** coin (and Bitcoin Core compatible chains).

Supports:
- CPU mining (SSE2 / AVX / AVX2)
- GPU mining (NVIDIA + AMD + Integrated)
- Multithreading with individual batch / bpg / tpb settings for each card and core
- Remote statistics reporting

Official site: [https://lcryp.com](https://lcryp.com)
> **📢 Official Telegram:** [**t.me/lcryp_com_official**](https://t.me/lcryp_com_official) — news, mining & community

---

## Requirements

- Windows 10/11 (x64)
- Python 3.8+ (recommended 3.11–3.13)
- Visual Studio 2022 (for building DLLs)
- CUDA Toolkit 12.x (if using NVIDIA)
- Running `lcryp-cli.exe` + `lcryp.conf` with RPC enabled

---

## Installation

```bash
pip install -r requirements.txt
```

---

## Building DLLs

All build `.bat` scripts are in the project root.

Examples:
```bat
build_avx2.bat
build_GpuModuleBridge.bat
build_GpuModuleGreen.bat
build_GpuModuleRed.bat
```

After building, the corresponding `.dll` files will appear in the same folder.

---

## Usage

### Basic commands

```bat
info.bat          — show available hardware
help.bat          — full parameter help
```

### Clean launch examples

#### CPU only (AVX2)
```bat
python LcRypSoloMiner.py ^
  --address "YOUR_ADDRESS" ^
  --cli "path\to\lcryp-cli.exe" ^
  --conf "path\to\lcryp.conf" ^
  --cpu_dll "LcRypMinerCoreAVX2.dll" ^
  --cpus "0,1,2,3,4,5,6,7" ^
  --cpu_batch "5000000" ^
  --name "CPU_AVX2"
```

#### NVIDIA only (Green)
```bat
python LcRypSoloMiner.py ^
  --address "YOUR_ADDRESS" ^
  --cli "path\to\lcryp-cli.exe" ^
  --conf "path\to\lcryp.conf" ^
  --gpu_dll "GpuModuleBridge.dll" ^
  --gpus_green "0,1" ^
  --gpu_batch_green "100,100" ^
  --gpu_bpg_green "4096,4096" ^
  --gpu_tpb_green "256,256" ^
  --name "GPU_NVIDIA"
```

#### AMD only (Red)
```bat
python LcRypSoloMiner.py ^
  --address "YOUR_ADDRESS" ^
  --cli "path\to\lcryp-cli.exe" ^
  --conf "path\to\lcryp.conf" ^
  --gpu_dll "GpuModuleBridge.dll" ^
  --gpus_red "0,1,2,3,4,5" ^
  --gpu_batch_red "1000" ^
  --gpu_bpg_red "4096" ^
  --gpu_tpb_red "256" ^
  --name "GPU_AMD"
```

#### Mixed mode (CPU + NVIDIA)
```bat
python LcRypSoloMiner.py ^
  --address "YOUR_ADDRESS" ^
  --cli "path\to\lcryp-cli.exe" ^
  --conf "path\to\lcryp.conf" ^
  --cpu_dll "LcRypMinerCoreAVX2.dll" ^
  --gpu_dll "GpuModuleBridge.dll" ^
  --cpus "0,1,2,3,4,5,6,7" ^
  --cpu_batch "5000000" ^
  --gpus_green "0,1" ^
  --gpu_batch_green "100,100" ^
  --gpu_bpg_green "4096,4096" ^
  --gpu_tpb_green "256,256" ^
  --name "CPU+GPU"
```

---

## Main parameters

| Parameter | Description |
|---------|------|
| `--address` | Payout address |
| `--cli` | Path to `lcryp-cli.exe` |
| `--conf` | Path to `lcryp.conf` |
| `--cpu_dll` | CPU DLL (AVX / AVX2 / SSE2) |
| `--gpu_dll` | GPU dispatcher DLL (`GpuModuleBridge.dll`) |
| `--cpus` | List of CPU cores |
| `--cpu_batch` | Batch size for each core |
| `--gpus_green` | NVIDIA card indices |
| `--gpus_red` | AMD card indices |
| `--gpus_integrated` | Integrated graphics indices |
| `--gpu_batch_*` / `--gpu_bpg_*` / `--gpu_tpb_*` | Parameters for each GPU type |
| `--remote_url` | Statistics URL (default: https://lcryp.com/worker_api.php) |
| `--api_pass` | API password |
| `--name` | Worker name |

Full help:
```bat
python LcRypSoloMiner.py --help
```

---

## Recommended `lcryp.conf`

```ini
server=1
rpcuser=your_username
rpcpassword=your_strong_password
rpcport=4994
rpctimeout=30
printtoconsole=1
listen=1
dbcache=4000
txindex=1
rpcworkqueue=100
```

---

## Notes

- Each GPU type has **independent indexing** starting from `0`.
- If a parameter list is shorter than the number of devices — the last value is repeated.
- NVIDIA requires CUDA Toolkit 12.x.
- It is recommended to run the miner as Administrator.

---

**LcRyp Solo Miner v2.2**  
https://lcryp.com
---

### 📢 Community & Support

Join the official Telegram channel for updates, tips and help:  
**[t.me/lcryp_com_official](https://t.me/lcryp_com_official)**

```
