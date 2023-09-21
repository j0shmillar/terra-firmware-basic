# Overview

This is a very simple server that will receive raw audio and write it to a file.
Samples are appended to any existing file so if you want to start again from scratch you will need to delete the raw file.

To import the files:
* Go to "File => Import => Raw Data" and select the RAW file
* "Encoding" = "Signed 16-bit PCM"
* "Byte order" = "Little-endian"
* "Channels" = "1 Channel (Mono)"
* "Sample rate" = 16000 Hz

# Prerequisites

You will need to have:
* [node](https://nodejs.org/en/download/)
* [yarn](https://classic.yarnpkg.com/en/docs/install/#mac-stable)

# Setup

To install the dependencies run:
```shell
$ cd server
$ yarn
```

# Running the server

The following commands will run the server:
```shell
$ cd server
$ yarn start
```
