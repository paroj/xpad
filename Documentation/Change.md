# Installing
```
sudo git clone https://github.com/paroj/xpad.git /usr/src/xpad-0.4
sudo dkms install -m xpad -v 0.4
```
# Updating
```
cd /usr/src/xpad-0.4
sudo git fetch
sudo git checkout origin/master
sudo dkms remove -m xpad -v 0.4 --all
sudo dkms install -m xpad -v 0.4
```
# Removing
```
sudo dkms remove -m xpad -v 0.4 --all
sudo rm -rf /usr/src/xpad-0.4
```