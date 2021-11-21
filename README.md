video management for file or stream

## features

* input
  * video file
  * GB28181
* output
  * RTSP
  * WebRTC

## preparation

* install docker
* login your docker hub account ( for store base image, ref [build-base.sh](tools/scripts/build-bash.sh) for details )

## build

```bash
cd ffvms
./tools/scripts/build-base.sh # only needed in first time
./tools/scripts/dev-env.sh # will start a backend container and listen ssh in host port 2222 with passwd 12345678
# then use vscode remote ssh to contact your dev env ( this dir will be mounted on /ffvms )
```

