
###### Setting Permissions
```bash
# Create the lights group and add the current user
sudo groupadd lights
sudo usermod -a -G lights $(whoami)

# Create the file file permissions
sudo cp ./adapter/udev/99-lights.rules /etc/udev/rules.d/99-lights.rules
sudo udevadm trigger --verbose --subsystem-match=lights

# Force the group into effect without logging out
su $(whoami)
```
