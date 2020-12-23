/*
 * Stegseek 0.4 - a steghide cracker
 * Copyright (C) 2020 Rick de Jager
 * 
 * Based on the work of Stefan Hetzl <shetzl@chello.at>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 */

#include "Cracker.h"

#include "error.h"
#include "Utils.h"
#include "common.h"
#include "EmbData.h"
#include "MHashPP.h"
#include "Selector.h"
#include "Extractor.h"
#include "CvrStgFile.h"

#include <cstring>


Cracker::Cracker ()
{
	VerboseMessage vrs ;
	// get stegofile
	if (Args.StgFn.getValue() != "") {
		vrs.setMessage (_("[v] Using stegofile \"%s\"."), Args.StgFn.getValue().c_str()) ;
	} else {
		vrs.setMessage (_("[v] Reading stegofile from stdin.")) ;
	}
	vrs.printMessage() ;

	// get output file
	if (Args.ExtFn.is_set()) {
		if (Args.ExtFn.getValue() != "") {
			vrs.setMessage (_("[v] Using output file \"%s\"."), Args.ExtFn.getValue().c_str()) ;
		} else {
			vrs.setMessage (_("[v] Using stdout as output.")) ;
		}
		vrs.printMessage() ;
	}

	// Print threading info
	vrs.setMessage (_("[v] Running on %d threads."), Args.Threads.getValue()) ;
	vrs.printMessage() ;

	vrs.setMessage ("") ;
	vrs.printMessage() ;

	// Load the Stegfile
	Globs.TheCvrStgFile = CvrStgFile::readFile (Args.StgFn.getValue().c_str()) ;

	// Look up these parameters once, to prevent function calls for each passphrase attempt
	bitsperembvalue = AUtils::log2_ceil<unsigned short> (Globs.TheCvrStgFile->getEmbValueModulus()) ;
	numSamples = Globs.TheCvrStgFile->getNumSamples() ;
	samplesPerVertex = Globs.TheCvrStgFile->getSamplesPerVertex() ;
	EmbValueModulus = Globs.TheCvrStgFile->getEmbValueModulus();
	embvaluesRequestedMagic = AUtils::div_roundup<unsigned long> (EmbData::NBitsMagic, bitsperembvalue) ;

	// Load the embedded values into a "plain" array for faster lookup
	embeddedValues = new EmbValue[numSamples] ;
	for (unsigned int i = 0; i < numSamples; i++) {
		embeddedValues[i] = Globs.TheCvrStgFile->getEmbeddedValue(i) ;
	}

	// init the attempts counter
	attempts = 0 ;
	stopped = false ;
	success = false ;
}

void Cracker::metrics (unsigned long max)
{
	while (!stopped)
	{
		unsigned long a = attempts ;
		float percentage = 100.0f * ((float) a / (float) max) ;
		printf("\r[ %lu / %lu ]  (%.2f%%)                 ",a , max, percentage) ;
		std::this_thread::sleep_for(std::chrono::milliseconds(10)) ; 
	}
	// Print a newline before returning
	puts("") ;
}

bool Cracker::verifyMagic (std::string Passphrase)
{	
	MHASH td = mhash_init(MHASH_MD5);
	mhash(td, Passphrase.data(), Passphrase.size() ) ;

	UWORD32 seed = 0;
	UWORD32 hash[4] ;
	mhash_deinit (td, hash) ;
	for (unsigned int i = 0 ; i < 4 ; i++) {
		seed ^= hash[i] ;
	}
	return verifyMagic(seed) ;
}

bool Cracker::verifyMagic (char * Passphrase)
{
	MHASH td = mhash_init(MHASH_MD5);
	//mhash(td, Passphrase, sizeof(Passphrase) / sizeof(char)) ;
	mhash(td, Passphrase, std::strlen(Passphrase)) ;

	UWORD32 seed = 0;
	UWORD32 hash[4] ;
	mhash_deinit (td, hash) ;
	for (unsigned int i = 0 ; i < 4 ; i++) {
		seed ^= hash[i] ;
	}
	return verifyMagic(seed) ;


}
bool Cracker::verifyMagic (UWORD32 seed)
{
	// Create a buf to keep track of rng collisions
	UWORD32 rngBuf[25*samplesPerVertex + 1] ;
	rngBuf[0] = seed ;

	// Pseudorandom properties
	const UWORD32 A = 1367208549 ;
	const UWORD32 C = 1 ;

	const int magics[25] = {
		// Magic, "shm" in binary LE
		1, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 1, 0,
		// Code version (0)
		0,
	} ;
	if (samplesPerVertex * embvaluesRequestedMagic >= numSamples) {
		// TODO; In theory, we should error out if we hit this.
		// However, I've seen this error happen randomly on valid input files
		// so I'm leaving it as a "return false" for now
		// throw CorruptDataError (_("the stego data from standard input is too short to contain the embedded data.")) ;
		return false ;
	}
	EmbValue ev = 0 ;
	unsigned long sv_idx = 0;
	for (unsigned long i = 0 ; i < embvaluesRequestedMagic ; i++) {
		for (unsigned int j = 0 ; j < samplesPerVertex ; j++, sv_idx++) {
			// Calc next random number
			rngBuf[sv_idx+1] = (UWORD32) (rngBuf[sv_idx]*A + C) ;
			// Check for RNG collisions. Should be fairly rare as numSample gets larger
			for (unsigned long k = 0; k <= sv_idx; k++) {
				if (rngBuf[k] == rngBuf[sv_idx+1]) {
					// In case we find an rng collision, just pretend the magic check was succesful.
					// The stricter steghide extractor will double check our work
					//
					// This case is rare
					return true;
				}
			}
			const UWORD32 valIdx = sv_idx + (((double) rngBuf[sv_idx+1] / (double) 4294967296.0) * ((double) (numSamples-sv_idx))) ;
			ev = (ev + embeddedValues[valIdx]) % EmbValueModulus;
		}
		if (ev != magics[i]) {
			return false ;
		}
		ev = 0;
	}
	return true;
}


void Cracker::extract (EmbData* emb)
{
	std::string origFn = emb->getFileName() ;
	std::string outFn = Utils::stripDir(Globs.TheCvrStgFile->getName()) + ".out" ;
	if (Args.ExtFn.is_set()) {
		outFn = Args.ExtFn.getValue() ;
	}

	if (origFn != "") {
		printf("[i] Original filename: \"%s\"\n", origFn.c_str()) ;
	}
	if (outFn != "") {
		printf("[i] Extracting to \"%s\"\n", outFn.c_str()) ;
	} else {
		printf("[i] Extracting to stdout\n") ;
	}
	BinaryIO io (outFn, BinaryIO::WRITE) ;
	std::vector<BYTE> data = emb->getData() ;
	for (std::vector<BYTE>::iterator i = data.begin() ; i != data.end() ; i++) {
		io.write8 (*i) ;
	}
	io.close() ;
}
