// DDSTextureLoader12.h
// Equivalent D3D12 de DDSTextureLoader.h (D3D11) -- meme contrat d'entree
// (octets bruts du fichier .dds, header compris -- voir CImageLoaderDDS::loadImage(),
// qui remplit CImage::CompressedSize avec la taille totale du fichier pour le chemin
// compresse), mais production d'un ID3D12Resource deja rempli (tous les mips/tranches
// de tableau/faces de cube uploades) plutot qu'un ID3D11Resource cree avec donnees
// initiales -- D3D12 n'a pas d'equivalent direct a ID3D11Device::CreateTexture2D(desc,
// initialData, ...), l'upload est fait explicitement via une resource UPLOAD +
// CopyTextureRegion par subresource (meme mecanisme que
// CD3D12Texture::uploadArraySlices()).
#pragma once

#include "IrrCompileConfig.h"
#ifdef _IRR_COMPILE_WITH_DIRECT3D_12_

#include <d3d12.h>
#include <wrl/client.h>
#include <cstddef>

namespace irr
{
	namespace video
	{
		using Microsoft::WRL::ComPtr;

		class CD3D12Driver;

		//! Resultat du chargement : la ressource GPU deja entierement uploadee (tous
		//! mips/tranches), plus les informations necessaires a CD3D12Texture pour finir
		//! sa propre initialisation (TextureType, ColorFormat, creation de la SRV via
		//! createShaderResourceView() -- ce loader ne cree PAS la vue lui-meme, il reste
		//! focalise sur la ressource + son contenu, comme CreateDDSTextureFromMemory le
		//! fait cote D3D11 en une seule passe -- ici la creation de vue est laissee a
		//! l'appelant pour reutiliser le code de vue deja generique de CD3D12Texture).
		struct SDDSTexture12Result
		{
			ComPtr<ID3D12Resource> Resource;
			DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
			UINT Width = 0;
			UINT Height = 0;
			UINT MipLevels = 0;
			//! Nombre de tranches de tableau/faces de cube (6 * nombre de cubes pour un
			//! cube map ou cube array) ; toujours 1 pour ETT_2D et ETT_3D (voir Dimension).
			UINT ArraySize = 0;
			bool IsCubeMap = false;
			D3D12_RESOURCE_DIMENSION Dimension = D3D12_RESOURCE_DIMENSION_UNKNOWN;
		};

		//! Parse un buffer .dds en memoire (octets bruts du fichier, header DDS_HEADER/
		//! DX10 compris) et cree + remplit un ID3D12Resource via la file d'upload
		//! synchrone du driver (CD3D12Driver::beginUpload()/endUploadAndWait(), meme
		//! mecanisme que le reste de CD3D12Texture -- pas de ResourceUploadBatch
		//! DirectXTK12, pour rester coherent avec l'upload "maison" deja utilise
		//! partout ailleurs dans ce driver). Retourne false (et logge une erreur) si le
		//! buffer n'est pas un .dds valide ou si la creation de la ressource echoue.
		bool CreateDDSTextureFromMemory12(CD3D12Driver* driver, const unsigned char* ddsData,
			size_t ddsDataSize, SDDSTexture12Result& outResult);
	}
}

#endif // _IRR_COMPILE_WITH_DIRECT3D_12_
