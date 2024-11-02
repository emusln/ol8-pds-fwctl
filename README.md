fwctl driver support for AMD/Pensando PDS Core


Steps to use:
    Note:
	These steps will change as the new modules stabilize
	These steps are probably incomplete and misleading,
	be careful out there!

1. Make sure you're on an Oracle 8.10 box running 5.15.0-301.163.5.2.el8uek.x86_64
	$ grep PRETTY /etc/os-release
	PRETTY_NAME="Oracle Linux Server 8.10"
	$ uname -r
	5.15.0-301.163.5.2.el8uek.x86_64

2. Clone this repo and branch
	git clone https://github.com/emusln/ol8-pds-fwctl/tree/ol8uek-pds-fwctl

3. Build the full Linux kernel with the fwctl commits in this branch
    copy the .config file
	cp /boot/config-5.15.0-301.163.5.2.el8uek.x86_64
    add CONFIG_FWCTL_PDS=m to .config, right after CONFIG_FWCTL_MLX5=m
    add something to CONFIG_LOCALVERSION= to make it your own kernel version string
    build new config and kernel
	make olddefconfig
	make -j 32
    install the kernel as roor
	make modules_install
	make install
    set the default boot kernel
    	grubby --set-default=0
    reboot

