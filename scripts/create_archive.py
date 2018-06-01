#!/usr/bin/env python
import click
import os
import sys

# TODO: set up proper package
test_dir = os.path.join(os.path.sep.join(os.path.realpath(__file__).split(os.path.sep)[:-2]), "tests/")
sys.path.append(test_dir)
from kotekan_runner import KotekanRunner

# (freq, prod, time)
DEFAULT_CHUNK = (16,16,16)
ERR_SQ_LIM = 3e-3
DATA_FIXED_PREC = 1e-4
WEIGHT_FIXED_PREC = 1e-3

@click.command()
@click.option("--log-level", default='info', help="default: info")
@click.option("--chunk", nargs=3, type=int, default=DEFAULT_CHUNK,
              help="[freq prod time] chunk. default: 16 16 16")
@click.argument("infile")
@click.argument("outfile")
def create_archive(infile, outfile, log_level, chunk):
    """ Transform kotekan receiver raw output file into transposed and bitshuffle
        compressed archive file.
    """

    bufs = {
        'read_buffer': {
            'kotekan_buffer': 'standard',
            'metadata_pool': 'vis_pool',
            'num_frames': 'buffer_depth',
            'sizeof_int': 4,
            'frame_size': '2 * sizeof_int * num_local_freq * num_elements * num_elements'
        },
        'trunc_buffer': {
            'kotekan_buffer': 'standard',
            'metadata_pool': 'vis_pool',
            'num_frames': 'buffer_depth',
            'sizeof_int': 4,
            'frame_size': '2 * sizeof_int * num_local_freq * num_elements * num_elements'
        }
    }

    config = { 'log_level': log_level, 'num_elements': 2048 }

    proc = {}
    # Reader process
    proc.update( {
            'read_raw': {
                'kotekan_process': 'visRawReader',
                'filename': os.path.abspath(infile),
                'chunk_size': chunk,
                'out_buf': 'read_buffer'
            }
        }
    )

    # Truncate process
    proc.update( {
            'truncate': {
                'kotekan_process': 'visTruncate',
                'err_sq_lim': ERR_SQ_LIM,
                'data_fixed_precision': DATA_FIXED_PREC,
                'weight_fixed_precision': WEIGHT_FIXED_PREC,
                'in_buf': 'read_buffer',
                'out_buf': 'trunc_buffer'
            }
        }
    )

    # Transpose/write process
    proc.update( {
            'transpose': {
                'kotekan_process': 'visTranspose',
                'in_buf': 'trunc_buffer',
                'chunk_size': chunk,
                'md_filename': os.path.abspath(infile) + '.meta',
                'filename': os.path.abspath(outfile)
            }
        }
    )

    runner = KotekanRunner(buffers=bufs, processes=proc, config=config)
    runner.run()

if __name__ == '__main__':
    create_archive()
