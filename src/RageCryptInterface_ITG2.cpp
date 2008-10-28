#include "global.h"
#include "RageLog.h"
#include "RageCryptInterfaceITG2.h"

#include "aes/aes.h"
#include "crypto/CryptSH512.h"
#include "ibutton/ibutton.h"

// we can safely assume that all instances of crypt_file
// will match ours, due to the crypt_create_internal call.
crypt_file *RageCryptInterface_ITG2::crypt_copy_internal( crypt_file *cf )
{
	crypt_file *cf_new = new crypt_file_ITG2( cf );

	// copy the ctx value
	memcpy( cf_new->ctx, cf->ctx, sizeof(cf->ctx) );

	return cf_new;
}

int RageCryptInterfaceITG2::crypt_open(crypt_file *newfile, CString secret)
{
	LOG->Debug( "RageCryptInterfaceITG2::crypt_open( %s )", newfile->path.c_str() );

	char header[2];
	unsigned char *subkey, verifyblock[16];
	size_t got, subkeysize;
	unsigned char *SHABuffer, *AESKey, plaintext[16], AESSHABuffer[64];

	if (read(newfile->fd, header, 2) < 2)
	{
		LOG->Warn("ITG2CryptInterface: Could not open %s: unexpected header size", name.c_str());
	}

	if (secret.length() == 0)
	{
		if (header[0] != ':' || header[1] != '|')
		{
			LOG->Warn("ITG2CryptInterface: no secret given and %s is not an ITG2 arcade encrypted file", name.c_str());
			return NULL;
		}
	}
	else
	{
		if (header[0] != '8' || header[1] != 'O')
		{
			LOG->Warn("ITG2CryptInterface: secret given, but %s is not an ITG2 patch file", name.c_str());
			return NULL;
		}
	}

	if (read(newfile->fd, &(newfile->filesize), 4) < 4)
	{
		LOG->Warn("ITG2CryptInterface: Could not open %s: unexpected file size", name.c_str());
		return NULL;
	}
	
	if (read(newfile->fd, &subkeysize, 4) < 4)
	{
		LOG->Warn("ITG2CryptInterface: Could not open %s: unexpected subkey size", name.c_str());
		return NULL;
	}
	
	subkey = new unsigned char[subkeysize];

	if ((got = read(newfile->fd, subkey, subkeysize)) < subkeysize)
	{
		LOG->Warn("ITG2CryptInterface: %s: subkey: expected %i, got %i", name.c_str(), subkeysize, got);
		return NULL;
	}

	if ((got = read(newfile->fd, verifyblock, 16)) < 16)
	{
		LOG->Warn("ITG2CryptInterface: %s: verifyblock: expected 16, got %i", name.c_str(), got);
		return NULL;
	}
	
	tKeyMap::iterator iter = ITG2CryptInterface::KnownKeys.find(name.c_str());
	if (iter != ITG2CryptInterface::KnownKeys.end())
	{
		CHECKPOINT_M("cache");
		AESKey = iter->second;
	}
	else
	{
		if (secret.length() > 0)
		{
			CHECKPOINT_M("patch");
			AESKey = new unsigned char[24];
			SHABuffer = new unsigned char[subkeysize + 47];
			memcpy(SHABuffer, subkey, subkeysize);
			memcpy(SHABuffer+subkeysize, secret.c_str(), 47);
			SHA512_Simple(SHABuffer, subkeysize+47, AESSHABuffer);
			memcpy(AESKey, AESSHABuffer, 24);
		}
		else
		{
			CHECKPOINT_M("dongle");
			AESKey = new unsigned char[24];
			GetKeyFromDongle(subkey, AESKey);
		}

		unsigned char *cAESKey = new unsigned char[24];
		memcpy(cAESKey, AESKey, 24);
		ITG2CryptInterface::KnownKeys[name.c_str()] = cAESKey;
	}
	delete subkey;

	aes_decrypt_key(AESKey, 24, newfile->ctx);
	aes_decrypt(verifyblock, plaintext, newfile->ctx);
	if (plaintext[0] != ':' || plaintext[1] != 'D')
	{
		LOG->Warn("ITG2CryptInterface: %s: decrypt failed, unexpected decryption magic", name.c_str());
		return NULL;
	}

	newfile->filepos = 0;
	newfile->headersize = lseek(newfile->fd, 0, SEEK_CUR);
	newfile->path = name;

	return newfile;
}

int ITG2CryptInterface::GetKeyFromDongle(const unsigned char *subkey, unsigned char *aeskey)
{
	return iButton::GetAESKey(subkey, aeskey);
}

int ITG2CryptInterface::crypt_read(crypt_file *cf, void *buf, int size)
{
	unsigned char backbuffer[16], decbuf[16], *crbuf, *dcbuf;

	size_t bufsize = ((size/16)+1)*16;

	if (size % 16 > 0)
		bufsize += 16;

	unsigned int oldpos = cf->filepos;

	crbuf = new unsigned char[bufsize]; // not an efficient way to do this, but oh well
	dcbuf = new unsigned char[bufsize];

	unsigned int cryptpos = (cf->filepos/16)*16; // position in file to make decryption ready
	unsigned int difference = cf->filepos - cryptpos;

	lseek( cf->fd, cf->headersize + cryptpos, SEEK_SET );
	read( cf->fd, crbuf, bufsize);

	if (cryptpos % 4080 == 0)
	{
		memset(backbuffer, '\0', 16);
	}
	else
	{
		lseek( cf->fd, cf->headersize + (cryptpos-16), SEEK_SET );
		read(cf->fd, backbuffer, 16);
	}

	for (unsigned i = 0; i < bufsize / 16; i++)
	{
		aes_decrypt(crbuf+(i*16), decbuf, cf->ctx);

		for (unsigned char j = 0; j < 16; j++)
			dcbuf[(i*16)+j] = decbuf[j] ^ (backbuffer[j] - j);

		if (((cryptpos + i*16) + 16) % 4080 == 0)
			memset(backbuffer, '\0', 16);
		else
			memcpy(backbuffer, crbuf+(i*16), 16);
	}

	memcpy(buf, dcbuf+difference, size);
	//memset(backbuffer, '\0', 16);
	//memcpy(backbuffer, dcbuf, 15);

	cf->filepos = oldpos + size;

	delete dcbuf;
	delete crbuf;
	return size;
}

int ITG2CryptInterface::crypt_seek(crypt_file *cf, int where)
{
	cf->filepos = where;
	return cf->filepos;
}

int ITG2CryptInterface::crypt_tell(crypt_file *cf)
{
	return cf->filepos;
}


int ITG2CryptInterface::crypt_close(crypt_file *cf)
{
	return close(cf->fd);
}

/*
 * Copyright (c) 2008 BoXoRRoXoRs
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, and/or sell copies of the Software, and to permit persons to
 * whom the Software is furnished to do so, provided that the above
 * copyright notice(s) and this permission notice appear in all copies of
 * the Software and that both the above copyright notice(s) and this
 * permission notice appear in supporting documentation.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
 * THIRD PARTY RIGHTS. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS
 * INCLUDED IN THIS NOTICE BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT
 * OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */