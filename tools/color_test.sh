#!/bin/sh
echo "TERM=$TERM"
echo "COLORTERM=$COLORTERM"
echo ""
printf '\e[34mANSI-BLUE\e[0m '
printf '\e[38;5;4mIDX-4\e[0m '
printf '\e[38;5;12mIDX-12\e[0m '
printf '\e[38;5;33mIDX-33\e[0m '
printf '\e[38;2;52;101;164mRGB-BLUE\e[0m '
printf '\e[38;2;59;130;246mRGB-BLUE2\e[0m\n'
