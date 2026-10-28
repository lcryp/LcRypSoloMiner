# mining_workers.py
import subprocess, json, ctypes, struct, time, binascii, hashlib, multiprocessing, argparse, sys, os, socket, threading, requests, traceback, secrets, psutil, inspect, re, portalocker
from multiprocessing import Lock, Process, shared_memory
from mining_utils import * 

def cpu_worker(cpu_id, cpu_batch_value, result_queue, args, shared_tpl, addr_hash, stats_dict, share_event, stop_event, tpl_lock):
    stats_key = f"CPU_{cpu_id}"
    try:  
        try:
            psutil.Process().cpu_affinity([cpu_id])
        except:
            pass         
        pc_name = socket.gethostname()
        seed_value = hash(f"{pc_name}_{time.time_ns()}_{cpu_id}")
        init_seed = calc_seed(seed_value)
        lib = ctypes.CDLL(args.cpu_dll)
        lib.run_mining_batch.argtypes = [
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint32)
        ]
        lib.run_mining_batch.restype = ctypes.c_uint32
        
        # --- CACHE INITIALIZATION (BEFORE LOOP) ---
        with tpl_lock:
            current_tpl = shared_tpl.copy()
        last_copy_time = time.time()


        while not stop_event.is_set():
            if not shared_tpl or 'height' not in shared_tpl:
                time.sleep(0.1)
                continue

            # --- TEMPLATE UPDATE (EVERY 10 SEC OR ON HEIGHT CHANGE) ---
            now = time.time()
            if (now - last_copy_time >= 10.0) or (shared_tpl.get('height') != current_tpl.get('height')):
                with tpl_lock:
                    current_tpl = shared_tpl.copy()
                last_copy_time = now

            header_data, h_u32, t_ptr = calc_header_data(args, current_tpl, addr_hash, stats_key)
            t0 = time.perf_counter()
            init_seed = calc_seed(seed_value + init_seed)
            found_nonce = lib.run_mining_batch(
                h_u32, 
                init_seed,      
                cpu_batch_value,
                t_ptr
            )
            _ = (h_u32, t_ptr, header_data)

            dt = time.perf_counter() - t0
            
            current_hr = float(cpu_batch_value) / 1000000.0 / max(0.00000001, dt)

            # Read previous value (if missing — take current)
            old_hr = stats_dict.get(stats_key, current_hr)
            
            # If value was 0 or first run — do not smooth sharply
            if old_hr == 0:
                new_hr = current_hr
            else:
                # Mathematical smoothing (EMA)
                new_hr = (old_hr * 0.8) + (current_hr * 0.2)

            stats_dict[stats_key] = new_hr

            if found_nonce: 
                stop_event.set() 
                header_data['found_nonce'] = found_nonce
                header_data['stats_key'] = stats_key
                result_queue.put(header_data)
                share_event.set()
                return  
    except (BrokenPipeError, EOFError, ConnectionError):
        # Just exit without errors
        return
    except Exception as e:
        error_details = traceback.format_exc()
        total_logger(f"CPU_WORKER {stats_key} ERROR:\n{error_details}", args.log)
        show_error_simple(f"[!] Error {stats_key}: {e.__class__.__name__}: {str(e)}")
        time.sleep(5)

class GPUMetrics(ctypes.Structure):
    _fields_ = [
        ("temperature", ctypes.c_int),
        ("fan_speed", ctypes.c_int),
        ("power_usage", ctypes.c_float),
        ("memory_used", ctypes.c_int),
        ("utilization", ctypes.c_int),
        ("clock_core", ctypes.c_int),
        ("clock_memory", ctypes.c_int),
        ("vendor_specific", ctypes.c_int)
    ]

def gpu_worker(gpu_type, gpu_id, gpu_batch, gpu_bpg, gpu_tpb, result_queue, args, shared_tpl, addr_hash, stats_dict, share_event, stop_event, tpl_lock):
    stats_key = f"GPU_{gpu_type}_{gpu_id}"
    try:
        pc_name = socket.gethostname()
        seed_value = hash(f"{pc_name}_{time.time_ns()}_{gpu_type}_{gpu_id}")
        init_seed = calc_seed(seed_value)
        lib = ctypes.CDLL(args.gpu_dll)

        lib.run_mining_batch_on_device.argtypes = [
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint32)
        ]
        lib.run_mining_batch_on_device.restype = ctypes.c_uint32
        
        metrics_func = None
        cleanup_func = None

        # Load metrics
        if hasattr(lib, 'get_gpu_full_stats'):
            try:
                lib.get_gpu_full_stats.restype = GPUMetrics
                lib.get_gpu_full_stats.argtypes = [ctypes.c_int]
                metrics_func = lib.get_gpu_full_stats
            except:
                metrics_func = None

        # Load cleanup function
        if hasattr(lib, 'cleanup_mining_drivers'):
            try:
                lib.cleanup_mining_drivers.restype = None
                lib.cleanup_mining_drivers.argtypes = []
                cleanup_func = lib.cleanup_mining_drivers
            except:
                cleanup_func = None

        # --- EMA PREPARATION AND STATS RECOVERY ---
        accumulated_hashes = 0
        window_start_time = time.perf_counter()
        old_stats = stats_dict.get(stats_key, {})
        ema_speed = old_stats.get("speed", 0.0) 
        is_first_valid_run = (ema_speed == 0.0) 
        metrics_available = metrics_func is not None
        
        # --- CACHE INITIALIZATION (BEFORE LOOP) ---
        with tpl_lock:
            current_tpl = shared_tpl.copy()
        last_copy_time = time.time()
        
        # Get global_id for this card
        try:
            gpu_lib = ctypes.CDLL(args.gpu_dll)
            gpu_global_id = gpu_lib.get_gpu_id(gpu_type, gpu_id)
        except:
            gpu_global_id = gpu_id  # fallback
        
        manager_alive = True
        while not stop_event.is_set():
            try:
                if not shared_tpl or 'height' not in shared_tpl:
                    time.sleep(0.1)
                    continue
                _ = len(shared_tpl)
            except (BrokenPipeError, EOFError, ConnectionError):
                manager_alive = False
                break

            # --- TEMPLATE UPDATE (EVERY 10 SEC OR ON HEIGHT CHANGE) ---
            now = time.time()
            if (now - last_copy_time >= 10.0) or (shared_tpl.get('height') != current_tpl.get('height')):
                with tpl_lock:
                    current_tpl = shared_tpl.copy()
                last_copy_time = now
            
            header_data, h_u32, t_ptr = calc_header_data(args, current_tpl, addr_hash, stats_key)
            t0 = time.perf_counter()
            init_seed = calc_seed(seed_value + init_seed)
            found_nonce = lib.run_mining_batch_on_device(
                gpu_global_id,
                h_u32,
                init_seed,
                gpu_bpg,
                gpu_tpb,
                gpu_batch,
                t_ptr
            )
            _ = (h_u32, t_ptr, header_data)
            dt = time.perf_counter() - t0 
            hashes_per_call = gpu_batch * gpu_bpg * gpu_tpb
            accumulated_hashes += hashes_per_call
            now = time.perf_counter()
            
            if now - window_start_time >= 2.0:
                raw_mhs = (accumulated_hashes / (now - window_start_time)) / 1000000.0
                if is_first_valid_run:
                    ema_speed = raw_mhs
                    if raw_mhs > 0: 
                        is_first_valid_run = False
                else:
                    ema_speed = (ema_speed * 0.8) + (raw_mhs * 0.2)

                # Get a copy of current data so we don't overwrite temperature/fans
                current_stats = dict(stats_dict.get(stats_key, {}))
                current_stats.update({
                    "speed": round(ema_speed, 2),
                    "batch": gpu_batch,
                    "bpg": gpu_bpg,
                    "tpb": gpu_tpb,
                    "last_update": time.time()
                }) 

                metrics_data = {}
                current_time = time.time() 
                if metrics_available:
                    try:
                        metrics = metrics_func(gpu_global_id)
                        metrics_data = {
                            "temperature": metrics.temperature if metrics.temperature != 0 else None,
                            "fan_speed": metrics.fan_speed if metrics.fan_speed != 0 else None,
                            "power_usage": round(metrics.power_usage, 1) if metrics.power_usage != 0.0 else None,
                            "memory_used": metrics.memory_used if metrics.memory_used != 0 else None,
                            "utilization": metrics.utilization if metrics.utilization != 0 else None,
                            "clock_core": metrics.clock_core if metrics.clock_core != 0 else None,
                            "clock_memory": metrics.clock_memory if metrics.clock_memory != 0 else None,
                            "vendor_specific": metrics.vendor_specific if metrics.vendor_specific != 0 else None
                        }
                    except Exception as e:
                        metrics_available = False
                        total_logger(f"GPU metrics error for GPU {gpu_type}:{gpu_id}: {e}", args.log)
                
                stats_data = {
                    "speed": round(ema_speed, 2),
                    "batch": gpu_batch,
                    "bpg": gpu_bpg,
                    "tpb": gpu_tpb,
                    "last_update": current_time
                }
                if metrics_data:
                    stats_data.update(metrics_data)

                stats_dict[stats_key] = stats_data
                accumulated_hashes = 0
                window_start_time = now

            if found_nonce:
                # Update only speed in existing dictionary
                final_stats = dict(stats_dict.get(stats_key, {}))
                final_stats.update({"speed": round(ema_speed, 2), "last_update": time.time()})
                stats_dict[stats_key] = final_stats
                stop_event.set()
                header_data['found_nonce'] = found_nonce
                header_data['stats_key'] = stats_key
                result_queue.put(header_data)
                share_event.set()
                return

        if cleanup_func and lib:
            try:
                cleanup_func()
            except:
                pass
    except (BrokenPipeError, EOFError, ConnectionError):
        # Just exit without errors
        return
    except Exception as e:
        error_details = traceback.format_exc()
        total_logger(f"GPU_WORKER [TYPE:{gpu_type}, ID:{gpu_id}] ERROR:\n{error_details}", args.log)
        show_error_simple(f"[!] Error GPU {gpu_type}:{gpu_id}: {e.__class__.__name__}: {str(e)}")
        if cleanup_func and lib:
            try:
                cleanup_func() 
            except Exception as cleanup_err:
                pass
        time.sleep(5)
    finally:
        # Always call cleanup
        if cleanup_func and lib:
            try:
                cleanup_func()
            except:
                pass

def start_workers(args, original_tpl, addr_hash, 
                  cpu_ids, cpu_batch_values,
                  gpu_ids_integrated, gpu_batch_integrated, gpu_bpg_integrated, gpu_tpb_integrated,
                  gpu_ids_green, gpu_batch_green, gpu_bpg_green, gpu_tpb_green,
                  gpu_ids_red, gpu_batch_red, gpu_bpg_red, gpu_tpb_red,
                  share_event, stop_event, manager, stats_dict, tpl_lock):
    
    result_queue = multiprocessing.Queue()
    procs = []
    with tpl_lock:
        shared_tpl = manager.dict()
        shared_tpl.update(original_tpl)
    
    # CPU workers
    if args.cpu_dll and cpu_ids:
        for idx, cpu_id in enumerate(cpu_ids):
            cpu_batch = cpu_batch_values[idx] if idx < len(cpu_batch_values) else cpu_batch_values[-1]

            p = multiprocessing.Process(
                target=cpu_worker, 
                args=(cpu_id, cpu_batch, result_queue, args, shared_tpl, addr_hash, stats_dict, share_event, stop_event, tpl_lock)
            )
            p.daemon = True
            p.start()
            procs.append(p)

    # GPU workers - separately for each type
    if args.gpu_dll:
        # Integrated GPU workers
        if gpu_ids_integrated:
            for idx, gpu_id in enumerate(gpu_ids_integrated):
                batch = gpu_batch_integrated[idx] if idx < len(gpu_batch_integrated) else gpu_batch_integrated[-1]
                bpg = gpu_bpg_integrated[idx] if idx < len(gpu_bpg_integrated) else gpu_bpg_integrated[-1]
                tpb = gpu_tpb_integrated[idx] if idx < len(gpu_tpb_integrated) else gpu_tpb_integrated[-1]
                
                p = multiprocessing.Process(
                    target=gpu_worker, 
                    args=(0, gpu_id, batch, bpg, tpb, result_queue, args, shared_tpl, addr_hash, stats_dict, share_event, stop_event, tpl_lock)
                )
                p.daemon = False
                p.start()
                procs.append(p)
        
        # Green GPU workers (NVIDIA)
        if gpu_ids_green:
            for idx, gpu_id in enumerate(gpu_ids_green):
                batch = gpu_batch_green[idx] if idx < len(gpu_batch_green) else gpu_batch_green[-1]
                bpg = gpu_bpg_green[idx] if idx < len(gpu_bpg_green) else gpu_bpg_green[-1]
                tpb = gpu_tpb_green[idx] if idx < len(gpu_tpb_green) else gpu_tpb_green[-1]
                
                p = multiprocessing.Process(
                    target=gpu_worker, 
                    args=(1, gpu_id, batch, bpg, tpb, result_queue, args, shared_tpl, addr_hash, stats_dict, share_event, stop_event, tpl_lock)
                )
                p.daemon = False
                p.start()
                procs.append(p)
        
        # Red GPU workers (AMD)
        if gpu_ids_red:
            for idx, gpu_id in enumerate(gpu_ids_red):
                batch = gpu_batch_red[idx] if idx < len(gpu_batch_red) else gpu_batch_red[-1]
                bpg = gpu_bpg_red[idx] if idx < len(gpu_bpg_red) else gpu_bpg_red[-1]
                tpb = gpu_tpb_red[idx] if idx < len(gpu_tpb_red) else gpu_tpb_red[-1]
                
                p = multiprocessing.Process(
                    target=gpu_worker, 
                    args=(2, gpu_id, batch, bpg, tpb, result_queue, args, shared_tpl, addr_hash, stats_dict, share_event, stop_event, tpl_lock)
                )
                p.daemon = False
                p.start()
                procs.append(p)

    if args.remote_url:
        monitor_thread = threading.Thread(
            target=report_to_server, 
            args=(args, stats_dict, share_event, stop_event), 
            daemon=True
        )
        monitor_thread.start()
    
    return result_queue, stats_dict, procs, shared_tpl
