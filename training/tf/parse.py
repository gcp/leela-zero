#!/usr/bin/env python3
#
#    This file is part of Leela Zero.
#    Copyright (C) 2017-2018 Gian-Carlo Pascutto
#
#    Leela Zero is free software: you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation, either version 3 of the License, or
#    (at your option) any later version.
#
#    Leela Zero is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License
#    along with Leela Zero.  If not, see <http://www.gnu.org/licenses/>.


from tfprocess import TFProcess
from chunkparser import ChunkParser
import argparse
import glob
import gzip
import multiprocessing as mp
import random
import shufflebuffer as sb
import sys
import tensorflow as tf
import time

# Sane values are from 4096 to 64 or so. The maximum depends on the amount
# of RAM in your GPU and the network size. You need to adjust the learning rate
# if you change this.
BATCH_SIZE = 512

# Use a random sample input data read. This helps improve the spread of
# games in the shuffle buffer.
DOWN_SAMPLE = 16

def get_chunks(data_prefix):
    return glob.glob(data_prefix + "*.gz")

class FileDataSrc:
    """
        data source yielding chunkdata from chunk files.
    """
    def __init__(self, chunks):
        self.chunks = chunks
        random.shuffle(self.chunks)
        self.done = []
    def next(self):
        if not self.chunks:
            self.chunks = self.done
            random.shuffle(self.chunks)
        if not self.chunks:
            return None
        filename = self.chunks.pop()
        self.done.append(filename)
        with gzip.open(filename, 'rb') as chunk_file:
            return chunk_file.read()

def benchmark(parser):
    """
        Benchmark for parser
    """
    gen = parser.parse()
    batch=100
    while True:
        start = time.time()
        for _ in range(batch):
            next(gen)
        end = time.time()
        print("{} pos/sec {} secs".format( BATCH_SIZE * batch / (end - start), (end - start)))

def benchmark1(t):
    """
        Benchmark for full input pipeline, including tensorflow conversion
    """
    batch=100
    while True:
        start = time.time()
        for _ in range(batch):
            t.session.run([t.next_batch],
                feed_dict={t.training: True, t.handle: t.train_handle})

        end = time.time()
        print("{} pos/sec {} secs".format( BATCH_SIZE * batch / (end - start), (end - start)))


def split_chunks(chunks, test_ratio):
    splitpoint = 1 + int(len(chunks) * (1.0 - test_ratio))
    return (chunks[:splitpoint], chunks[splitpoint:])

def _parse_function(planes, probs, winner):
    planes = tf.decode_raw(planes, tf.uint8)
    probs = tf.decode_raw(probs, tf.float32)
    winner = tf.decode_raw(winner, tf.float32)

    planes = tf.to_float(planes)

    planes = tf.reshape(planes, (BATCH_SIZE, 18, 19*19))
    probs = tf.reshape(probs, (BATCH_SIZE, 19*19 + 1))
    winner = tf.reshape(winner, (BATCH_SIZE, 1))

    return (planes, probs, winner)

def main():
    parser = argparse.ArgumentParser(description='Train network from game data.')
    parser.add_argument("trainpref", help='Training file prefix', nargs='?', type=str)
    parser.add_argument("restorepref", help='Training snapshot prefix', nargs='?', type=str)
    parser.add_argument("--train", '-t', help="Training file prefix", type=str)
    parser.add_argument("--test", help="Test file prefix", type=str)
    parser.add_argument("--restore", help="Prefix of tensorflow snapshot to restore from", type=str)
    args = parser.parse_args()

    train_data_prefix = args.train or args.trainpref
    restore_prefix = args.restore or args.restorepref

    training = get_chunks(train_data_prefix)
    if not args.test:
        # Generate test by taking 10% of the training chunks.
        random.shuffle(training)
        training, test = split_chunks(training, 0.1)
    else:
        test = get_chunks(args.test)

    if not training:
        print("No data to train on!")
        return

    print("Training with {0} chunks, validating on {1} chunks".format(
        len(training), len(test)))

    train_parser = ChunkParser(FileDataSrc(training),
            shuffle_size=1<<20, sample=DOWN_SAMPLE, batch_size=BATCH_SIZE)
    #benchmark(train_parser)
    dataset = tf.data.Dataset.from_generator(
        train_parser.parse, output_types=(tf.string, tf.string, tf.string))
    dataset = dataset.map(_parse_function)

    dataset = dataset.prefetch(4)
    train_iterator = dataset.make_one_shot_iterator()

    test_parser = ChunkParser(FileDataSrc(test), batch_size=BATCH_SIZE)
    dataset = tf.data.Dataset.from_generator(
        test_parser.parse, output_types=(tf.string, tf.string, tf.string))
    dataset = dataset.map(_parse_function)

    dataset = dataset.prefetch(4)
    test_iterator = dataset.make_one_shot_iterator()

    tfprocess = TFProcess()
    tfprocess.init(dataset, train_iterator, test_iterator)

    #benchmark1(tfprocess)

    if restore_prefix:
        tfprocess.restore(restore_prefix)
    while True:
        tfprocess.process(BATCH_SIZE)

if __name__ == "__main__":
    mp.set_start_method('spawn')
    main()
    mp.freeze_support()
