#!/usr/bin/env python3
"""
Histogram Equalization + Global Otsu Thresholding for E-ink Display
Converts low-contrast images to clean binary (black/white) output.

Usage:
    ./hist_eq_otsu.py input.png output.png [--show-histograms]
"""

import sys
import numpy as np
from PIL import Image
import argparse


def rgb_to_grayscale(rgb_img):
    """Convert RGB to grayscale using ITU-R BT.601 weights."""
    if len(rgb_img.shape) == 2:
        # Already grayscale
        return rgb_img
    return np.dot(rgb_img[...,:3], [0.299, 0.587, 0.114]).astype(np.uint8)


def histogram_equalization(img):
    """
    Apply histogram equalization to spread pixel values across full 0-255 range.

    Algorithm:
    1. Compute histogram and CDF
    2. Normalize CDF to 0-255 range
    3. Use CDF as lookup table: new_value = CDF[old_value]
    """
    # Compute histogram (256 bins)
    hist, bins = np.histogram(img.flatten(), 256, [0, 256])

    # Compute cumulative distribution function
    cdf = hist.cumsum()

    # Normalize CDF to 0-255 range
    # Remove zeros to avoid division by zero
    cdf_min = cdf[cdf > 0].min() if np.any(cdf > 0) else 0
    cdf_normalized = ((cdf - cdf_min) * 255 / (cdf[-1] - cdf_min)).astype(np.uint8)

    # Use CDF as lookup table
    img_equalized = cdf_normalized[img]

    return img_equalized


def otsu_threshold(img):
    """
    Find optimal Otsu threshold by maximizing between-class variance.

    Algorithm:
    1. For each possible threshold t in 0-255:
       - Split pixels into background (≤t) and foreground (>t)
       - Compute weighted variance between the two classes
    2. Return threshold that maximizes this variance

    Returns: (threshold, between_class_variance)
    """
    # Compute histogram
    hist, bins = np.histogram(img.flatten(), 256, [0, 256])
    hist = hist.astype(float)

    # Normalize histogram to probability distribution
    hist /= hist.sum()

    # Cumulative sums
    weight_bg = np.cumsum(hist)          # P(background) for each threshold
    weight_fg = 1.0 - weight_bg          # P(foreground) for each threshold

    # Cumulative means
    cum_mean = np.cumsum(hist * np.arange(256))
    mean_total = cum_mean[-1]

    # Avoid division by zero
    with np.errstate(divide='ignore', invalid='ignore'):
        mean_bg = cum_mean / weight_bg
        mean_fg = (mean_total - cum_mean) / weight_fg

    # Between-class variance for each threshold
    variance_between = weight_bg * weight_fg * (mean_bg - mean_fg) ** 2

    # Replace NaN/Inf with 0
    variance_between = np.nan_to_num(variance_between)

    # Find threshold that maximizes variance
    threshold = np.argmax(variance_between)
    max_variance = variance_between[threshold]

    return threshold, max_variance


def apply_threshold(img, threshold):
    """Apply binary threshold: pixel > threshold → 255 (white), else → 0 (black)."""
    return (img > threshold).astype(np.uint8) * 255


def print_histogram_stats(img, title="Image"):
    """Print histogram statistics."""
    hist, bins = np.histogram(img.flatten(), 256, [0, 256])

    print(f"\n{title} Statistics:")
    print(f"  Min: {img.min()}, Max: {img.max()}, Mean: {img.mean():.1f}, Std: {img.std():.1f}")
    print(f"  Total pixels: {img.size}")

    # Print histogram by 32-value bins
    print(f"\n  Histogram (32-value bins):")
    for i in range(0, 256, 32):
        count = hist[i:i+32].sum()
        pct = 100.0 * count / img.size
        bar = '█' * int(pct / 2)  # 1 char = 2%
        print(f"    {i:3d}-{i+31:3d}: {count:6d} pixels ({pct:5.1f}%) {bar}")


def plot_histograms(original, equalized, binary, threshold):
    """Plot histograms side-by-side (requires matplotlib)."""
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not available — skipping histogram plots")
        return

    fig, axes = plt.subplots(2, 2, figsize=(12, 8))

    # Original histogram
    axes[0, 0].hist(original.flatten(), 256, [0, 256], color='gray')
    axes[0, 0].set_title('Original Histogram')
    axes[0, 0].set_xlabel('Luma')
    axes[0, 0].set_ylabel('Pixel Count')

    # Equalized histogram
    axes[0, 1].hist(equalized.flatten(), 256, [0, 256], color='blue')
    axes[0, 1].axvline(threshold, color='red', linestyle='--', label=f'Otsu T={threshold}')
    axes[0, 1].set_title('Equalized Histogram + Otsu Threshold')
    axes[0, 1].set_xlabel('Luma')
    axes[0, 1].legend()

    # Original image
    axes[1, 0].imshow(original, cmap='gray')
    axes[1, 0].set_title('Original Grayscale')
    axes[1, 0].axis('off')

    # Binary result
    axes[1, 1].imshow(binary, cmap='gray')
    axes[1, 1].set_title(f'Binary Output (Threshold={threshold})')
    axes[1, 1].axis('off')

    plt.tight_layout()
    plt.show()


def main():
    parser = argparse.ArgumentParser(description='Histogram Equalization + Otsu for E-ink')
    parser.add_argument('input', help='Input image file')
    parser.add_argument('output', help='Output binary image file')
    parser.add_argument('--show-histograms', action='store_true',
                        help='Show histogram plots (requires matplotlib)')
    parser.add_argument('--skip-equalization', action='store_true',
                        help='Skip histogram equalization (apply Otsu directly)')
    args = parser.parse_args()

    # Load image
    print(f"Loading {args.input}...")
    img = Image.open(args.input)
    img_array = np.array(img)

    # Convert to grayscale
    gray = rgb_to_grayscale(img_array)
    print(f"Image size: {gray.shape[1]}×{gray.shape[0]} pixels")

    # Print original stats
    print_histogram_stats(gray, "Original")

    # Histogram equalization
    if args.skip_equalization:
        print("\nSkipping histogram equalization (--skip-equalization)")
        gray_eq = gray
    else:
        print("\nApplying histogram equalization...")
        gray_eq = histogram_equalization(gray)
        print_histogram_stats(gray_eq, "Equalized")

    # Otsu thresholding
    print("\nComputing Otsu threshold...")
    threshold, variance = otsu_threshold(gray_eq)
    print(f"  Optimal threshold: {threshold}")
    print(f"  Between-class variance: {variance:.4f}")

    # Apply threshold
    binary = apply_threshold(gray_eq, threshold)

    # Stats
    white_pixels = np.sum(binary == 255)
    black_pixels = np.sum(binary == 0)
    white_pct = 100.0 * white_pixels / binary.size
    black_pct = 100.0 * black_pixels / binary.size
    print(f"\nBinary output:")
    print(f"  White pixels: {white_pixels:6d} ({white_pct:5.1f}%)")
    print(f"  Black pixels: {black_pixels:6d} ({black_pct:5.1f}%)")

    # Save result
    print(f"\nSaving to {args.output}...")
    Image.fromarray(binary).save(args.output)
    print("Done!")

    # Show plots if requested
    if args.show_histograms:
        plot_histograms(gray, gray_eq, binary, threshold)


if __name__ == '__main__':
    main()
