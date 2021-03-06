/***

Copyright (C) 2015, 2016 Teclib'

This file is part of Armadito core.

Armadito core is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Armadito core is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with Armadito core.  If not, see <http://www.gnu.org/licenses/>.

***/

#include <libarmadito\armadito.h>
#include "armadito-config.h"

#include "core/mimetype.h"
#include "string_p.h"
#include "core/io.h"

#include <Windows.h>
#include <stdio.h>


#define MIME_SIZE 100
#define BUF_SIZE 1024

const char* getHresError(HRESULT hr)
{
	switch (hr){
		case S_OK: return "S_OK";
		case E_FAIL: return "E_FAIL";
		case E_INVALIDARG: return "E_INVALIDARG";
		case E_OUTOFMEMORY: return "E_OUTOFMEMORY";
		default: return "UNKNOWN_ERROR";
	}
}

const char *os_mime_type_guess_fd(int fd)
{
	char *mime_type;
	LPWSTR mt = 0;
	size_t i = 0;
	int n_read = 0;
	char *buf[BUF_SIZE];
	LPCWSTR defaultMime = L"*";

	if (fd < 0){
		printf("Invalid file descriptor %d",  fd);
		return NULL;
	}

	if ((n_read = _read(fd, buf, BUF_SIZE)) < 0) {
		a6o_log(A6O_LOG_LIB, A6O_LOG_LEVEL_WARNING, "Cannot read %d bytes from file descriptor %d", BUF_SIZE, fd);
		printf("Cannot read %d bytes from file descriptor %d", BUF_SIZE, fd);
		return NULL;
	}

	HRESULT res = FindMimeFromData(NULL, NULL, buf, BUF_SIZE, NULL, FMFD_DEFAULT, &mt, 0);
	if (res != S_OK) {
		printf("Error :: FindMimeFromData failed :: %s \n", getHresError(res));
		return NULL;
	}

	// convert wchar * to char * 
	mime_type = (char*)calloc(MIME_SIZE + 1, sizeof(char));
	mime_type[MIME_SIZE] = '\0';
	wcstombs_s(&i, mime_type, MIME_SIZE, (wchar_t*)mt, MIME_SIZE);

	// printf("mime_type = %s \n", mime_type);

	return mime_type;
}


const char *os_mime_type_guess(const char *path)
{
	char *mime_type;
	HANDLE fh;
	size_t size = 0;
	void * buf = NULL;
	LPWSTR mt = 0;
	LPDWORD high = NULL;
	DWORD read = 0;
	size_t i = 0;
	BOOL bres = FALSE;
	LARGE_INTEGER fileSize;

	fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ , NULL,OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,NULL );
	if (fh == INVALID_HANDLE_VALUE) {
		return NULL;
	}

	// get file content size in bytes.
	bres = GetFileSizeEx(fh, &fileSize);

	if (bres == FALSE) {
		a6o_log(A6O_LOG_LIB, A6O_LOG_LEVEL_WARNING, "Error :: os_mime_type_guess() :: GetFileSizeEx() failed :: %s :: err = %d (%s) :: ",path,GetLastError(),os_strerror(GetLastError()));
		return NULL;
	}

	size = fileSize.QuadPart;

	if (size > BUF_SIZE) {		
		size = BUF_SIZE;
	}

	buf = (char*)calloc(size + 1, sizeof(char));
	((char*)buf)[size] = '\0';

	if (ReadFile(fh, buf, size, &read, NULL) == FALSE) {
		a6o_log(A6O_LOG_LIB, A6O_LOG_LEVEL_WARNING, " os_mime_type_guess() :: Read file failed ::  (%s) ",os_strerror(GetLastError()));
		free(buf);
		CloseHandle(fh);
		return NULL; 
	}

	HRESULT res = FindMimeFromData(NULL, NULL, buf, size, NULL, FMFD_DEFAULT, &mt, 0);
	if ( res != S_OK) {	  
		a6o_log(A6O_LOG_LIB, A6O_LOG_LEVEL_ERROR," FindMimeFromData failed :: %s \n", getHresError(res));
		free(buf);
		CloseHandle(fh);
		return NULL;
	}

	// convert wchar * to char * 
	mime_type = (char*)calloc(MIME_SIZE+1,sizeof(char));
	mime_type[MIME_SIZE] = '\0';
	wcstombs_s(&i, mime_type, MIME_SIZE,(wchar_t*)mt, MIME_SIZE);

	free(buf);
	CloseHandle(fh);
	
	return mime_type;
}

void os_mime_type_init(void) {

	// This function is empty in windows version.
	return;
}
