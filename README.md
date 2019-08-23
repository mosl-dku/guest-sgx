## Prerequisites

There are a few tools that are necessary in order to build your own kernel(s). The 'git' (or 'git-core' for 10.04 or before) package provides the git revision control system which will be used to clone the mainline git repository. The 'kernel-package' provides the make-kpkg utility which automatically build your kernel and generate the linux-image and linux-header .deb files which can be installed. You will need to install the following packages:
    
    
    sudo apt-get install git build-essential kernel-package fakeroot libncurses5-dev libssl-dev ccache bison flex
    
    
## Kernel Build and Installation

#### 1.  Clone the mainline kernel git tree:


    GUEST="${HOME}"/guest
    git clone https://github.com/mosl-dku/guest-sgx.git "${GUEST}"/guest-sgx

    
#### 2.  Change to directory linux:

    cd "${GUEST}"/guest-sgx
    git checkout yeo

    
#### 3.  Copy the kernel config file from your existing system to the kernel tree:

    cp /boot/config-`uname -r` .config

#### 4.  Bring the config file up to date. Answer any questions that get prompted. Unless you know you are interested in a particular             feature, accepting the default option by pressing Enter should be a safe choice:

    make oldconfig

In cases where your kernel source is significantly newer than the existing config file, you'll be presented with all of the new         config options for which there is no existing config file setting. You can either sit there and keep hitting Enter to take the           default (generally safe), or you can just run:

    yes '' | make oldconfig

which emulates exactly the same thing and saves you all that time.
#### 5.  (optional) If you need to make any kernel config changes, do the following and save your changes when prompted:

    make menuconfig

#### 6.  Clean the kernel source directory:

    make clean

#### 7.  Build the linux-image and linux-header .deb files using a thread per core + 1. This process takes a lot of time:

    make -j `getconf _NPROCESSORS_ONLN` deb-pkg LOCALVERSION=-custom

With this command the package names will be something like linux-image-2.6.24-rc5-custom and linux-headers-2.6.24-rc5-custom, and in    that case the version will be 2.6.24-rc5-custom-10.00.Custom. You may change the string "custom" into something else by changing the    LOCALVERSION option.

#### 8.  Change to one directory level up (this is where the linux-image and linux-header .deb files were put) and send it to Guest-VM:

    cd ..
    scp linux-image-{VERSION}.deb linux-image-{VERSION}.deb {account of guest-vm}@{ip address of vm}:{directory}

    
#### 9.  Now install the .deb files. In the guest vm: 

    sudo dpkg -i linux-image-{VERSION}.deb
    sudo dpkg -i linux-headers-{VERSION}.deb

#### 10. You are now ready to boot into your new kernel. Just make sure you select the new kernel when you boot:

    sudo reboot

    
#### from https://wiki.ubuntu.com/KernelTeam/GitKernelBuild

