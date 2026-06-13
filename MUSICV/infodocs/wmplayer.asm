; WM Player
; (c) Grom PE / -=CHE@TER=- 2009
;
; Todo:
; - load musicv.com automatically

	org	100h

	mov	al, [80h]	; Check command line, show usage help if empty
	test	al, al
	jz	no_cmdline

	push	0
	pop	ds
        mov	ecx, [0D0h*4]	; Check interrupt vector
	push	cs
	pop	ds
;	jecxz	no_driver
	test	ecx, ecx
	jz	no_driver

	mov	bx, 80h
	movzx	si, byte [bx]
	mov	[bx+si+1], byte 0	; Put ending zero in filename in command line
	mov	dx, 82h
	mov	ax, 3D00h		; DOS: Open file for reading
	int	21h
	jc	file_open_error
	mov	[_file_handle], ax

	xchg	bx, ax		; File handle
	xor	cx, cx
	xor	dx, dx		; cx:dx - offset
 	mov	ax, 4202h	; DOS: Seek to end of file
	int	21h
	jc	file_work_error
	mov	[_file_length], ax	; dx:ax - file length (we ignore dx)

	mov	bx, [_file_handle]
	xor	cx, cx
	xor	dx, dx
	mov	ax, 4200h	; DOS: Seek to start of file
	int	21h
	jc	file_work_error

	mov	bx, [_file_handle]
	mov	cx, [_file_length]
	mov	dx, _memory_buff
	mov	ah, 3Fh		; DOS: Read file
	int	21h
	push	cs
	pop	ds
	jc	file_work_error

 	mov	bx, [_file_handle]
	mov	ah, 3Eh		; DOS: Close file
	int	21h

	; Call musicv.com driver
	push	cs
	pop	es
	mov	bx, _memory_buff	; es:bx - offset to data
	mov	al, 0h			; Play command
	int	0D0h

	; wait for keypress
	xor     ah, ah
	int     16h

	; al values:
	; 0 - play buffer
	; 1 - restart playing for current buffer
	; 2 - stop playing
	; 3 - ??? set something: if (flag & 0x06) { flag |= 0x20; }
	; 4,5,6,7 - stubs error (just set carry flag and return)
	; FF - unload driver as "MUSICV.COM -R" command
	; stop play
	mov    al, 2
	int    0D0h

	jmp	exit

no_driver:
	mov	dx, _error_no_driver
	mov	ah, 9		; DOS: Show message
	int	21h
	jmp	exit

no_cmdline:
	mov	dx, _usage
	mov	ah, 9		; DOS: Show message
	int	21h
	jmp	exit

file_open_error:
	mov	dx, _file_open_error
	mov	ah, 9		; DOS: Show message
	int	21h
	jmp	exit

file_work_error:
	mov	dx, _file_work_error
	mov	ah, 9		; DOS: Show message
	int	21h
;	jmp	exit

exit:
	mov	ah, 4Ch		; DOS: Exit
	int	21h

_usage           db 'Usage: wmplayer.com <filename.wm>',13,10,'$'
_file_open_error db 'Error: Couldn''t open file.',13,10,'$'
_file_work_error db 'Error: File reading failed.',13,10,'$'
_error_no_driver db 'Error: musicv.com driver must be loaded first!',13,10,'$'

_file_handle rw 1
_file_length rw 1

_memory_buff:
