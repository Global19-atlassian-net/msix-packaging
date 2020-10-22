//
//  Copyright (C) 2019 Microsoft.  All rights reserved.
//  See LICENSE file in the project root for full license information.
// 

#include "AppxPackaging.hpp"
#include "AppxPackageWriter.hpp"
#include "MsixErrors.hpp"
#include "Exceptions.hpp"
#include "ContentType.hpp"
#include "Encoding.hpp"
#include "ZipObjectWriter.hpp"
#include "AppxManifestObject.hpp"
#include "ScopeExit.hpp"
#include "FileNameValidation.hpp"
#include "StringHelper.hpp"

#include <string>
#include <memory>
#include <future>
#include <algorithm>
#include <functional>

#include <zlib.h>

namespace MSIX {

    AppxPackageWriter::AppxPackageWriter(IMsixFactory* factory, const ComPtr<IZipWriter>& zip, bool isBundle)
        : m_factory(factory), m_zipWriter(zip), m_isBundle(isBundle)
    {
        m_state = WriterState::Open;
        if (isBundle)
        {
            
        }
    }

    // IPackageWriter
    void AppxPackageWriter::PackPayloadFiles(const ComPtr<IDirectoryObject>& from)
    {
        ThrowErrorIf(Error::InvalidState, m_state != WriterState::Open, "Invalid package writer state");
        auto failState = MSIX::scope_exit([this]
        {
            this->m_state = WriterState::Failed;
        });

        auto fileMap = from->GetFilesByLastModDate();
        for(const auto& file : fileMap)
        {
            // If any footprint file is present, ignore it. We only require the AppxManifest.xml
            // and any other will be ignored and a new one will be created for the package. 
            if(!(FileNameValidation::IsFootPrintFile(file.second, m_isBundle) || FileNameValidation::IsReservedFolder(file.second)))
            {
                std::string ext = Helper::tolower(file.second.substr(file.second.find_last_of(".") + 1));
                auto contentType = ContentType::GetContentTypeByExtension(ext);
                auto stream = from.As<IStorageObject>()->GetFile(file.second);
                ValidateAndAddPayloadFile(file.second, stream.Get(), contentType.GetCompressionOpt(), contentType.GetContentType().c_str());
            }
        }
        failState.release();
    }

    // IPackageWriter
    void AppxPackageWriter::ProcessBundlePayload(const ComPtr<IDirectoryObject>& from, bool flatBundle)
    {
        ThrowErrorIf(Error::InvalidState, m_state != WriterState::Open, "Invalid package writer state");
        auto failState = MSIX::scope_exit([this]
            {
                this->m_state = WriterState::Failed;
            });

        auto fileMap = from->GetFilesByLastModDate();
        for (const auto& file : fileMap)
        {
            if (!(FileNameValidation::IsFootPrintFile(file.second, m_isBundle) || FileNameValidation::IsReservedFolder(file.second)))
            {
                std::string ext = Helper::tolower(file.second.substr(file.second.find_last_of(".") + 1));
                auto contentType = ContentType::GetContentTypeByExtension(ext);
                auto stream = from.As<IStorageObject>()->GetFile(file.second);

                if (flatBundle)
                {
                    ThrowHrIfFailed(AddPackageReference(utf8_to_wstring(file.second).c_str(), stream.Get(), false));
                }
            }
        }
        failState.release();
    }

    // IAppxPackageWriter
    HRESULT STDMETHODCALLTYPE AppxPackageWriter::AddPayloadFile(LPCWSTR fileName, LPCWSTR contentType,
        APPX_COMPRESSION_OPTION compressionOption, IStream *inputStream) noexcept try
    {
        return AddPayloadFile(wstring_to_utf8(fileName).c_str(), wstring_to_utf8(contentType).c_str(), 
            compressionOption, inputStream);
    } CATCH_RETURN();

    HRESULT STDMETHODCALLTYPE AppxPackageWriter::Close(IStream* manifest) noexcept try
    {
        if (m_isBundle) { return static_cast<HRESULT>(Error::PackageIsBundle); }

        ComPtr<IStream> manifestStream(manifest);

        // Process AppxManifest.xml
        // If the creating the AppxManifestObject succeeds, then the stream is valid.
        auto manifestObj = ComPtr<IAppxManifestReader>::Make<AppxManifestObject>(m_factory.Get(), manifestStream.Get());
        auto manifestContentType = ContentType::GetPayloadFileContentType(APPX_FOOTPRINT_FILE_TYPE_MANIFEST);
        AddFileToPackage(APPXMANIFEST_XML, manifestStream.Get(), true, true, manifestContentType.c_str());

        CloseInternal();
        return static_cast<HRESULT>(Error::OK);
    } CATCH_RETURN();

    // IAppxPackageWriterUtf8
    HRESULT STDMETHODCALLTYPE AppxPackageWriter::AddPayloadFile(LPCSTR fileName, LPCSTR contentType,
        APPX_COMPRESSION_OPTION compressionOption, IStream* inputStream) noexcept try
    {
        ThrowErrorIf(Error::InvalidState, m_state != WriterState::Open, "Invalid package writer state");
        auto failState = MSIX::scope_exit([this]
        {
            this->m_state = WriterState::Failed;
        });
        ComPtr<IStream> stream(inputStream);
        ValidateAndAddPayloadFile(fileName, stream.Get(), compressionOption, contentType);
        failState.release();
        return static_cast<HRESULT>(Error::OK);
    } CATCH_RETURN();

    // IAppxPackageWriter3
    HRESULT STDMETHODCALLTYPE AppxPackageWriter::AddPayloadFiles(UINT32 fileCount,
        APPX_PACKAGE_WRITER_PAYLOAD_STREAM* payloadFiles, UINT64 memoryLimit) noexcept try
    {
        ThrowErrorIf(Error::InvalidState, m_state != WriterState::Open, "Invalid package writer state");
            auto failState = MSIX::scope_exit([this]
        {
            this->m_state = WriterState::Failed;
        });
        // TODO: use memoryLimit for how many files are going to be added
        for(UINT32 i = 0; i < fileCount; i++)
        {
            std::string fileName = wstring_to_utf8(payloadFiles[i].fileName);
            ComPtr<IStream> stream(payloadFiles[i].inputStream);
            std::string contentType = wstring_to_utf8(payloadFiles[i].contentType);
            ValidateAndAddPayloadFile(fileName, stream.Get(), payloadFiles[i].compressionOption, contentType.c_str());
        }
        failState.release();
        return static_cast<HRESULT>(Error::OK);
    } CATCH_RETURN();

    // IAppxPackageWriter3Utf8
    HRESULT STDMETHODCALLTYPE AppxPackageWriter::AddPayloadFiles(UINT32 fileCount,
        APPX_PACKAGE_WRITER_PAYLOAD_STREAM_UTF8* payloadFiles, UINT64 memoryLimit) noexcept try
    {
        ThrowErrorIf(Error::InvalidState, m_state != WriterState::Open, "Invalid package writer state");
        auto failState = MSIX::scope_exit([this]
        {
            this->m_state = WriterState::Failed;
        });
        // TODO: use memoryLimit for how many files are going to be added
        for(UINT32 i = 0; i < fileCount; i++)
        {
            ComPtr<IStream> stream(payloadFiles[i].inputStream);
            ValidateAndAddPayloadFile(payloadFiles[i].fileName, stream.Get(), payloadFiles[i].compressionOption, payloadFiles[i].contentType);
        }
        failState.release();
        return static_cast<HRESULT>(Error::OK);
    } CATCH_RETURN();

    // IAppxBundleWriter
    HRESULT STDMETHODCALLTYPE AppxPackageWriter::AddPayloadPackage(LPCWSTR fileName, IStream* packageStream) noexcept try
    {
        // TODO: implement
        NOTIMPLEMENTED;
    } CATCH_RETURN();

    HRESULT STDMETHODCALLTYPE AppxPackageWriter::Close() noexcept try
    {
        if (!m_isBundle) { return static_cast<HRESULT>(Error::NotImplemented); }

        //ComPtr<IStream> manifestStream(manifest);

        // Process AppxBundleManifest.xml
        // If the creating the AppxManifestObject succeeds, then the stream is valid.
        /*auto manifestObj = ComPtr<IAppxManifestReader>::Make<AppxManifestObject>(m_factory.Get(), manifestStream.Get());
        auto manifestContentType = ContentType::GetPayloadFileContentType(APPX_BUNDLE_FOOTPRINT_FILE_TYPE_MANIFEST);
        AddFileToPackage(APPXBUNDLEMANIFEST_XML, manifestStream.Get(), true, true, manifestContentType.c_str());*/

        CloseInternal();
        return static_cast<HRESULT>(Error::OK);
    } CATCH_RETURN();

    // IAppxBundleWriter4
    HRESULT STDMETHODCALLTYPE AppxPackageWriter::AddPackageReference(LPCWSTR fileName, 
        IStream* inputStream, BOOL isDefaultApplicablePackage) noexcept try
    {        
        HRESULT hr = this->AddPackageReferenceInternal(fileName, inputStream, !!isDefaultApplicablePackage);

        if (FAILED(hr))
        {
            //fail
        }

        return hr;

    } CATCH_RETURN();

    HRESULT AppxPackageWriter::AddPackageReferenceInternal(
        _In_ LPCWSTR fileName,
        _In_ IStream* packageStream,
        _In_ bool isDefaultApplicablePackage)
    {
        auto appxFactory = m_factory.As<IAppxFactory>();

        ComPtr<IAppxPackageReader> reader;
        ThrowHrIfFailed(appxFactory->CreatePackageReader(packageStream, &reader));

        /*if (!this->blockMapInitialized)
        {

        }*/

        UINT64 packageStreamSize = 0;
        ThrowHrIfFailed(GetStreamSize(packageStream, &packageStreamSize));

        ThrowHrIfFailed(AddPackage(fileName, reader.Get(), 0, packageStreamSize, isDefaultApplicablePackage, false));

        return S_OK;
    }

    HRESULT AppxPackageWriter::GetStreamSize(_In_ IStream* stream, _Out_ UINT64* sizeOfStream)
    {
        HRESULT hr = S_OK;
        STATSTG stat;
        ThrowHrIfFailed(stream->Stat(&stat, STATFLAG_NONAME));

        *sizeOfStream = stat.cbSize.QuadPart;
        return S_OK;
    }

    HRESULT AppxPackageWriter::AddPackage(
        _In_ PCWSTR fileName,
        _In_ IAppxPackageReader* packageReader,
        _In_ UINT64 bundleOffset,
        _In_ UINT64 packageSize,
        _In_ bool isDefaultApplicableResource,
        _In_ bool isStub)
    {
        ComPtr<IAppxManifestPackageId> packageId;
        APPX_BUNDLE_PAYLOAD_PACKAGE_TYPE packageType = APPX_BUNDLE_PAYLOAD_PACKAGE_TYPE_APPLICATION;
        ComPtr<IAppxManifestQualifiedResourcesEnumerator> resources;
        ComPtr<IAppxManifestTargetDeviceFamiliesEnumerator> tdfs;

        ThrowHrIfFailed(GetValidatedPackageData(fileName, packageReader, isStub, /*&packageType*/
            &packageId, &resources, &tdfs));

        //auto innerPackageIdInternal = packageId.As<IAppxManifestPackageIdInternal>();

        ThrowHrIfFailed(AddValidatedPackageData(fileName, bundleOffset, packageSize, packageType, packageId,
                isDefaultApplicableResource, resources.Get(), tdfs.Get(), isStub));
        return S_OK;

    }

    HRESULT AppxPackageWriter::GetValidatedPackageData(
        _In_ PCWSTR fileName,
        _In_ IAppxPackageReader* packageReader,
        _In_ bool isStub,
        /*_Out_ APPX_BUNDLE_PAYLOAD_PACKAGE_TYPE* packageType,*/
        _Outptr_result_nullonfailure_ IAppxManifestPackageId** packageId,
        _Outptr_result_nullonfailure_ IAppxManifestQualifiedResourcesEnumerator** resources,
        _Outptr_result_maybenull_ IAppxManifestTargetDeviceFamiliesEnumerator** tdfs)
    {
        ComPtr<IAppxManifestPackageId> loadedPackageId;
        APPX_BUNDLE_PAYLOAD_PACKAGE_TYPE loadedPackageType = APPX_BUNDLE_PAYLOAD_PACKAGE_TYPE_APPLICATION;
        ComPtr<IAppxManifestQualifiedResourcesEnumerator> loadedResources;
        ComPtr<IAppxManifestTargetDeviceFamiliesEnumerator> loadedTdfs;

        ComPtr<IAppxManifestReader> manifestReader;
        ThrowHrIfFailed(packageReader->GetManifest(&manifestReader));
        ThrowHrIfFailed(manifestReader->GetPackageId(&loadedPackageId));
        //AutoCoTaskMemString packageFullName;
        //ThrowHrIfFailed(loadedPackageId->GetPackageFullName(&packageFullName));
        //ComPtr<IAppxManifestReader3> manifestReader3;
        //ThrowHrIfFailed(manifestReader.As(&manifestReader3));
        //ThrowHrIfFailed(manifestReader3->GetQualifiedResources(&loadedResources));

        //a lot more validations

        //*packageType = loadedPackageType;
        *packageId = loadedPackageId.Detach();

        return S_OK;

    }

    HRESULT AppxPackageWriter::AddValidatedPackageData(
    _In_ PCWSTR fileName,
    _In_ UINT64 bundleOffset,
    _In_ UINT64 packageSize,
    _In_ APPX_BUNDLE_PAYLOAD_PACKAGE_TYPE packageType,
    _In_ ComPtr<IAppxManifestPackageId> packageId,
    _In_ BOOL isDefaultApplicablePackage,
    _In_ IAppxManifestQualifiedResourcesEnumerator* resources,
    _In_ IAppxManifestTargetDeviceFamiliesEnumerator* tdfs,
    _In_ bool isStub)
    {
        //validate package payload extension

        auto innerPackageIdInternal = packageId.As<IAppxManifestPackageIdInternal>();

        PackageInfo packageInfo;
        packageInfo.type = packageType;
        packageInfo.architecture = innerPackageIdInternal->GetArchitecture();
        ThrowHrIfFailed(packageId->GetVersion(&packageInfo.version));
        packageInfo.resourceId = innerPackageIdInternal->GetResourceId();
        packageInfo.isDefaultApplicablePackage = isDefaultApplicablePackage;
        packageInfo.resources = resources;
        packageInfo.fileName = fileName;
        packageInfo.size = packageSize;
        packageInfo.offset = bundleOffset;
        packageInfo.tdfs = tdfs;
        packageInfo.isStub = isStub;

        ThrowHrIfFailed(AddPackageInfoToVector(packageInfo));

        return S_OK;
    }

    HRESULT AppxPackageWriter::AddPackageInfoToVector(_In_ PackageInfo packageInfo)
    {
        this->payloadPackages.push_back(packageInfo);
        //More checks
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE AppxPackageWriter::AddPayloadPackage(LPCWSTR fileName, IStream* packageStream, 
        BOOL isDefaultApplicablePackage) noexcept try
    {
        // TODO: implement
        NOTIMPLEMENTED;
    } CATCH_RETURN();

    HRESULT STDMETHODCALLTYPE AppxPackageWriter::AddExternalPackageReference(LPCWSTR fileName,
        IStream* inputStream, BOOL isDefaultApplicablePackage) noexcept try
    {
        // TODO: implement
        NOTIMPLEMENTED;
    } CATCH_RETURN();

    void AppxPackageWriter::ValidateAndAddPayloadFile(const std::string& name, IStream* stream,
        APPX_COMPRESSION_OPTION compressionOpt, const char* contentType)
    {
        ThrowErrorIfNot(Error::InvalidParameter, FileNameValidation::IsFileNameValid(name), "Invalid file name");
        ThrowErrorIf(Error::InvalidParameter, FileNameValidation::IsFootPrintFile(name, m_isBundle), "Trying to add footprint file to package");
        ThrowErrorIf(Error::InvalidParameter, FileNameValidation::IsReservedFolder(name), "Trying to add file in reserved folder");
        ValidateCompressionOption(compressionOpt);
        AddFileToPackage(name, stream, compressionOpt != APPX_COMPRESSION_OPTION_NONE, true, contentType);
    }

    void AppxPackageWriter::AddFileToPackage(const std::string& name, IStream* stream, bool toCompress,
        bool addToBlockMap, const char* contentType, bool forceContentTypeOverride)
    {
        std::string opcFileName;
        // Don't encode [Content Type].xml
        if (contentType != nullptr)
        {
            opcFileName = Encoding::EncodeFileName(name);
        }
        else
        {
            opcFileName = name;
        }
        auto fileInfo = m_zipWriter->PrepareToAddFile(opcFileName, toCompress);

        // Add content type to [Content Types].xml
        if (contentType != nullptr)
        {
            m_contentTypeWriter.AddContentType(name, contentType, forceContentTypeOverride);
        }

        // This might be called with external IStream implementations. Don't rely on internal implementation of FileStream
        LARGE_INTEGER start = { 0 };
        ULARGE_INTEGER end = { 0 };
        ThrowHrIfFailed(stream->Seek(start, StreamBase::Reference::END, &end));
        ThrowHrIfFailed(stream->Seek(start, StreamBase::Reference::START, nullptr));
        std::uint64_t uncompressedSize = static_cast<std::uint64_t>(end.QuadPart);

        // Add file to block map.
        if (addToBlockMap)
        {
            m_blockMapWriter.AddFile(name, uncompressedSize, fileInfo.first);
        }

        auto& zipFileStream = fileInfo.second;

        std::uint64_t bytesToRead = uncompressedSize;
        std::uint32_t crc = 0;
        while (bytesToRead > 0)
        {
            // Calculate the size of the next block to add
            std::uint32_t blockSize = (bytesToRead > DefaultBlockSize) ? DefaultBlockSize : static_cast<std::uint32_t>(bytesToRead);
            bytesToRead -= blockSize;

            // read block from stream
            std::vector<std::uint8_t> block;
            block.resize(blockSize);
            ULONG bytesRead;
            ThrowHrIfFailed(stream->Read(static_cast<void*>(block.data()), static_cast<ULONG>(blockSize), &bytesRead));
            ThrowErrorIfNot(Error::FileRead, (static_cast<ULONG>(blockSize) == bytesRead), "Read stream file failed");
            crc = crc32(crc, block.data(), static_cast<uInt>(block.size()));

            // Write block and compress if needed
            ULONG bytesWritten = 0;
            ThrowHrIfFailed(zipFileStream->Write(block.data(), static_cast<ULONG>(block.size()), &bytesWritten));

            // Add block to blockmap
            if (addToBlockMap)
            {
                m_blockMapWriter.AddBlock(block, bytesWritten, toCompress);
            }

        }

        if (toCompress)
        {
            // Put the stream termination on
            std::vector<std::uint8_t> buffer;
            ULONG bytesWritten = 0;
            ThrowHrIfFailed(zipFileStream->Write(buffer.data(), static_cast<ULONG>(buffer.size()), &bytesWritten));
        }

        // Close File element
        if (addToBlockMap)
        {
            m_blockMapWriter.CloseFile();
        }

        // This could be the compressed or uncompressed size
        auto streamSize = zipFileStream.As<IStreamInternal>()->GetSize();
        m_zipWriter->EndFile(crc, streamSize, uncompressedSize, true);
    }

    void AppxPackageWriter::ValidateCompressionOption(APPX_COMPRESSION_OPTION compressionOpt)
    {
        bool result = ((compressionOpt == APPX_COMPRESSION_OPTION_NONE) ||
                       (compressionOpt == APPX_COMPRESSION_OPTION_NORMAL) ||
                       (compressionOpt == APPX_COMPRESSION_OPTION_MAXIMUM) ||
                       (compressionOpt == APPX_COMPRESSION_OPTION_FAST) ||
                       (compressionOpt == APPX_COMPRESSION_OPTION_SUPERFAST));
        ThrowErrorIfNot(Error::InvalidParameter, result, "Invalid compression option.");
    }

    // Common close functionality for pack and bundle.
    // Adds AppxBlockMap.xml, [Content_Types].xml and closes the zip object.
    void AppxPackageWriter::CloseInternal()
    {
        ThrowErrorIf(Error::InvalidState, m_state != WriterState::Open, "Invalid package writer state");
        auto failState = MSIX::scope_exit([this]
            {
                this->m_state = WriterState::Failed;
            });

        // Close blockmap and add it to package
        m_blockMapWriter.Close();
        auto blockMapStream = m_blockMapWriter.GetStream();
        auto blockMapContentType = ContentType::GetPayloadFileContentType(APPX_FOOTPRINT_FILE_TYPE_BLOCKMAP);
        AddFileToPackage(APPXBLOCKMAP_XML, blockMapStream.Get(), true, false, blockMapContentType.c_str());

        // Close content types and add it to package
        m_contentTypeWriter.Close();
        auto contentTypeStream = m_contentTypeWriter.GetStream();
        AddFileToPackage(CONTENT_TYPES_XML, contentTypeStream.Get(), true, false, nullptr);

        m_zipWriter->Close();
        failState.release();
        m_state = WriterState::Closed;
    }

}
