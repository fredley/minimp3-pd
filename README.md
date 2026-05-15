minimp3 for Playdate
==========

[minimp3](https://github.com/lieff/minimp3) is a minimalistic, single-header library for decoding MP3. This fork has minor modifications to get it working on a Playdate, with an example app included.

## Why?

The playdate's built in mp3 decoder is very limited, and has quite a few subtle bugs and incompatibilities. Having attempted to build an mp3 streaming app around it, I found using minimp3 provided a _much_ more stable experience.

## The Changes

The change required to get minimp3 running on Playdate hardware is to move the per-frame working buffer from the stack to static storage, since it is too large to fit on the Playdate's stack.

## The Example App

Included is an example app that plays the BBC World Service stream. This stream cannot be played directly as it is not at 44.1khz, so it is resampled on the fly.

```
cd app
make run     # run in the simulator
make device  # build for the device
```