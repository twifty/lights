sudo groupadd lights
sudo usermod -a -G lights owen
sudo cp ./udev/99-lights.rules /etc/udev/rules.d/99-lights.rules
sudo udevadm trigger --verbose --subsystem-match=lights
su owen # forces the group into effect
