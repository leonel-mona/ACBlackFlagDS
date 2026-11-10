#!/usr/bin/env python3
"""
Analyze DirectStorageProxy.log for I/O patterns.

Parses [IO] ReadFile lines and generates a summary report.
"""

import re
import os
import sys
from collections import defaultdict
from pathlib import Path


def parse_log_line(line):
    """Parse a single [IO] ReadFile log line."""
    # Format: [IO] ReadFile path="..." offset=<int> length=<int> status=<hex> bytes=<int>
    pattern = r'\[IO\] ReadFile path="([^"]+)" offset=(\d+) length=(\d+) status=(0x[0-9a-fA-F]+) bytes=(\d+)'
    match = re.match(pattern, line.strip())
    if not match:
        return None
    
    path, offset, length, status, bytes_read = match.groups()
    return {
        'path': path,
        'offset': int(offset),
        'length': int(length),
        'status': status,
        'bytes_read': int(bytes_read)
    }


def format_bytes(bytes_val):
    """Format bytes to human readable string."""
    if bytes_val >= 1024**3:
        return f"{bytes_val / (1024**3):.2f} GB"
    elif bytes_val >= 1024**2:
        return f"{bytes_val / (1024**2):.2f} MB"
    elif bytes_val >= 1024:
        return f"{bytes_val / 1024:.2f} KB"
    else:
        return f"{bytes_val} B"


def analyze_log(log_path):
    """Analyze the log file and return statistics."""
    forge_reads = []  # List of dicts for .forge files
    file_stats = defaultdict(lambda: {'bytes': 0, 'count': 0, 'reads': []})
    
    total_ops = 0
    total_requested = 0
    total_read = 0
    
    with open(log_path, 'r', encoding='utf-8', errors='ignore') as f:
        for line in f:
            parsed = parse_log_line(line)
            if not parsed:
                continue
            
            total_ops += 1
            total_requested += parsed['length']
            total_read += parsed['bytes_read']
            
            # Only track .forge files
            if '.forge' in parsed['path'].lower():
                forge_reads.append(parsed)
                filename = os.path.basename(parsed['path'])
                file_stats[filename]['bytes'] += parsed['bytes_read']
                file_stats[filename]['count'] += 1
                file_stats[filename]['reads'].append(parsed)
    
    return {
        'total_ops': total_ops,
        'total_requested': total_requested,
        'total_read': total_read,
        'forge_reads': forge_reads,
        'file_stats': file_stats
    }


def calculate_size_distribution(forge_reads):
    """Calculate read size distribution."""
    bins = {
        '<=64KB': 0,
        '64KB-512KB': 0,
        '512KB-1MB': 0,
        '>1MB': 0
    }
    
    for read in forge_reads:
        size = read['bytes_read']
        if size <= 64 * 1024:
            bins['<=64KB'] += 1
        elif size <= 512 * 1024:
            bins['64KB-512KB'] += 1
        elif size <= 1024 * 1024:
            bins['512KB-1MB'] += 1
        else:
            bins['>1MB'] += 1
    
    total = sum(bins.values())
    result = {}
    for k, v in bins.items():
        pct = (v / total * 100) if total > 0 else 0
        result[k] = {'count': v, 'percentage': pct}
    
    return result


def calculate_sequential_ratio(forge_reads):
    """Calculate sequential read ratio per file."""
    # Group reads by file
    file_reads = defaultdict(list)
    for read in forge_reads:
        filename = os.path.basename(read['path'])
        file_reads[filename].append(read)
    
    total_sequential = 0
    total_reads_with_prev = 0
    
    for filename, reads in file_reads.items():
        # Sort by offset
        reads_sorted = sorted(reads, key=lambda x: x['offset'])
        
        for i in range(1, len(reads_sorted)):
            prev = reads_sorted[i - 1]
            curr = reads_sorted[i]
            
            expected_offset = prev['offset'] + prev['bytes_read']
            # Allow small gap (<= 256KB) for sequential
            if curr['offset'] == expected_offset or (curr['offset'] > expected_offset and curr['offset'] - expected_offset <= 256 * 1024):
                total_sequential += 1
            total_reads_with_prev += 1
    
    ratio = (total_sequential / total_reads_with_prev * 100) if total_reads_with_prev > 0 else 0
    return {
        'sequential_count': total_sequential,
        'total_comparable': total_reads_with_prev,
        'ratio_percent': ratio
    }


def get_top_files(file_stats, by='bytes', top_n=10):
    """Get top N files by bytes or count."""
    sorted_files = sorted(
        file_stats.items(),
        key=lambda x: x[1][by],
        reverse=True
    )
    return sorted_files[:top_n]


def generate_report(stats, output_path):
    """Generate the analysis markdown report."""
    forge_reads = stats['forge_reads']
    file_stats = stats['file_stats']
    
    size_dist = calculate_size_distribution(forge_reads)
    seq_stats = calculate_sequential_ratio(forge_reads)
    top_by_bytes = get_top_files(file_stats, 'bytes', 10)
    top_by_count = get_top_files(file_stats, 'count', 10)
    
    lines = []
    lines.append("# I/O Analysis Report")
    lines.append("")
    lines.append(f"**Log file:** DirectStorageProxy.log")
    lines.append(f"**Total .forge read operations:** {len(forge_reads)}")
    lines.append(f"**Total requested:** {format_bytes(stats['total_requested'])} ({stats['total_requested']:,} bytes)")
    lines.append(f"**Total bytes read:** {format_bytes(stats['total_read'])} ({stats['total_read']:,} bytes)")
    lines.append("")
    
    # Size distribution
    lines.append("## Read Size Distribution")
    lines.append("")
    lines.append("| Size Range | Count | Percentage |")
    lines.append("|------------|-------|------------|")
    for size_range, data in size_dist.items():
        lines.append(f"| {size_range} | {data['count']} | {data['percentage']:.1f}% |")
    lines.append("")
    
    # Sequential ratio
    lines.append("## Sequential Read Ratio")
    lines.append("")
    lines.append(f"- **Sequential reads:** {seq_stats['sequential_count']}")
    lines.append(f"- **Total comparable reads:** {seq_stats['total_comparable']}")
    lines.append(f"- **Sequential ratio:** {seq_stats['ratio_percent']:.1f}%")
    lines.append("")
    
    # Top 10 by bytes
    lines.append("## Top 10 .forge Files by Total Bytes Read")
    lines.append("")
    lines.append("| Rank | File | Total Bytes | Total MB | Read Count |")
    lines.append("|------|------|-------------|----------|------------|")
    for i, (filename, data) in enumerate(top_by_bytes, 1):
        lines.append(f"| {i} | {filename} | {data['bytes']:,} | {data['bytes'] / (1024**2):.2f} | {data['count']} |")
    lines.append("")
    
    # Top 10 by count
    lines.append("## Top 10 .forge Files by Read Count")
    lines.append("")
    lines.append("| Rank | File | Read Count | Total Bytes | Total MB |")
    lines.append("|------|------|------------|-------------|----------|")
    for i, (filename, data) in enumerate(top_by_count, 1):
        lines.append(f"| {i} | {filename} | {data['count']} | {data['bytes']:,} | {data['bytes'] / (1024**2):.2f} |")
    lines.append("")
    
    # File details
    lines.append("## All .forge Files Summary")
    lines.append("")
    lines.append("| File | Read Count | Total Bytes | Total MB |")
    lines.append("|------|------------|-------------|----------|")
    for filename, data in sorted(file_stats.items(), key=lambda x: x[1]['bytes'], reverse=True):
        lines.append(f"| {filename} | {data['count']} | {data['bytes']:,} | {data['bytes'] / (1024**2):.2f} |")
    
    report = '\n'.join(lines)
    
    # Write to file
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, 'w', encoding='utf-8') as f:
        f.write(report)
    
    return report


def main():
    log_path = Path(__file__).parent.parent / "DirectStorageProxy.log"
    output_path = Path(__file__).parent.parent / "reports" / "analysis.md"
    
    if not log_path.exists():
        print(f"Error: Log file not found at {log_path}")
        sys.exit(1)
    
    print(f"Analyzing {log_path}...")
    stats = analyze_log(log_path)
    report = generate_report(stats, output_path)
    
    print(report)
    print(f"\nReport saved to {output_path}")


if __name__ == "__main__":
    main()