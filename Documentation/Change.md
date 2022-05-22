
## Installing

```sh
sudo git clone                          \
    https://github.com/paroj/xpad.git   \
    /usr/src/xpad-0.4

sudo dkms install   \
    -m xpad         \
    -v 0.4
```

## Updating

```sh
cd /usr/src/xpad-0.4

sudo git fetch
sudo git checkout origin/master

sudo dkms remove    \
    -m xpad         \
    -v 0.4          \
    --all
    
sudo dkms install   \
    -m xpad         \
    -v 0.4
```

## Removing

```sh
sudo dkms remove    \
    -m xpad         \
    -v 0.4          \
    --all
    
sudo rm             \
    --recursive     \
    --force         \
    /usr/src/xpad-0.4
```