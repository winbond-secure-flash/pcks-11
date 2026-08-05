FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

# Replace ipl_burning.py with W77Q-aware version (adds chip-erase before write)
SRC_URI:append = " file://ipl_burning.py"
