#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <time.h>

#include <sys/types.h>
#include <sys/stat.h>

#ifdef __linux__
#include <limits.h>
#elif defined (__MACH__)
#include <sys/param.h>
#endif

#include <dlib/dstrings.h>
#include <dlib/hash.h>
#include <dlib/hashtable.h>
#include <dlib/log.h>
#include <dlib/http_client.h>
#include <dlib/http_server.h>
#include <dlib/uri.h>
#include <dlib/profile.h>

#include "resource.h"
#include "resource_archive.h"

/*
 * TODO:
 *
 *  - Resources could be loaded twice if canonical path is different for equivalent files.
 *    We could use realpath or similar function but we want to avoid file accesses when converting
 *    a canonical path to hash value. This functionality is used in the GetDescriptor function.
 *
 *  - If GetCanonicalPath exceeds RESOURCE_PATH_MAX PATH_TOO_LONG should be returned.
 *
 *  - Handle out of resources. Eg Hashtables full.
 */

namespace dmResource
{

struct SResourceType
{
    SResourceType()
    {
        memset(this, 0, sizeof(*this));
    }
    const char*       m_Extension;
    void*             m_Context;
    FResourceCreate   m_CreateFunction;
    FResourceDestroy  m_DestroyFunction;
    FResourceRecreate m_RecreateFunction;
};

// This is both for the total resource path, ie m_UriParts.X concatenated with relative path
const uint32_t RESOURCE_PATH_MAX = 1024;

const uint32_t MAX_RESOURCE_TYPES = 128;
const uint32_t MAX_CALLBACKS = 16;

static FactoryResult LoadResource(HFactory factory, const char* path, const char* original_name, uint32_t* resource_size);

struct ResourceReloadedCallbackPair
{
    ResourceReloadedCallback    m_Callback;
    void*                       m_UserData;
};

struct SResourceFactory
{
    // TODO: Arg... budget. Two hash-maps. Really necessary?
    dmHashTable<uint64_t, SResourceDescriptor>*  m_Resources;
    dmHashTable<uintptr_t, uint64_t>*            m_ResourceToHash;
    // Only valid if RESOURCE_FACTORY_FLAGS_RELOAD_SUPPORT is set
    // Used for reloading of resources
    dmHashTable<uint64_t, const char*>*          m_ResourceHashToFilename;
    // Only valid if RESOURCE_FACTORY_FLAGS_RELOAD_SUPPORT is set
    dmArray<ResourceReloadedCallbackPair>*       m_ResourceReloadedCallbacks;
    SResourceType                                m_ResourceTypes[MAX_RESOURCE_TYPES];
    uint32_t                                     m_ResourceTypesCount;

    dmURI::Parts                                 m_UriParts;
    dmHttpClient::HClient                        m_HttpClient;

    char*                                        m_StreamBuffer;
    uint32_t                                     m_StreamBufferSize;

    // HTTP related state
    // Total number bytes loaded in current GET-request
    uint32_t                                     m_HttpContentLength;
    uint32_t                                     m_HttpTotalBytesStreamed;
    int                                          m_HttpStatus;
    FactoryResult                                m_HttpFactoryResult;

    // HTTP server
    dmHttpServer::HServer                        m_HttpServer;

    // Resource archive
    dmResourceArchive::HArchive                  m_BuiltinsArchive;
};

static SResourceType* FindResourceType(SResourceFactory* factory, const char* extension)
{
    for (uint32_t i = 0; i < factory->m_ResourceTypesCount; ++i)
    {
        SResourceType* rt = &factory->m_ResourceTypes[i];
        if (strcmp(extension, rt->m_Extension) == 0)
        {
            return rt;
        }
    }
    return 0;
}

// TODO: Test this...
static void GetCanonicalPath(const char* base_dir, const char* relative_dir, char* buf)
{
    DM_SNPRINTF(buf, RESOURCE_PATH_MAX, "%s/%s", base_dir, relative_dir);

    char* source = buf;
    char* dest = buf;
    char last_c = 0;
    while (*source != 0)
    {
        char c = *source;
        if (c != '/' || (c == '/' && last_c != '/'))
            *dest++ = c;

        last_c = c;
        ++source;
    }
    *dest = '\0';
}

void SetDefaultNewFactoryParams(struct NewFactoryParams* params)
{
    params->m_MaxResources = 1024;
    params->m_Flags = RESOURCE_FACTORY_FLAGS_EMPTY;
    params->m_StreamBufferSize = 4 * 1024 * 1024;
    params->m_BuiltinsArchive = 0;
    params->m_BuiltinsArchiveSize = 0;
}

static void HttpHeader(dmHttpClient::HClient client, void* user_data, int status_code, const char* key, const char* value)
{
    SResourceFactory* factory = (SResourceFactory*) user_data;
    factory->m_HttpStatus = status_code;

    if (strcmp(key, "Content-Length") == 0)
    {
        factory->m_HttpContentLength = strtol(value, 0, 10);
    }
}

static void HttpContent(dmHttpClient::HClient, void* user_data, int status_code, const void* content_data, uint32_t content_data_size)
{
    SResourceFactory* factory = (SResourceFactory*) user_data;
    (void) status_code;

    assert(factory->m_HttpTotalBytesStreamed <= factory->m_StreamBufferSize);
    if (factory->m_StreamBufferSize - factory->m_HttpTotalBytesStreamed < content_data_size)
    {
        factory->m_HttpFactoryResult = FACTORY_RESULT_STREAMBUFFER_TOO_SMALL;
        return;
    }

    memcpy(factory->m_StreamBuffer + factory->m_HttpTotalBytesStreamed, content_data, content_data_size);
    factory->m_HttpTotalBytesStreamed += content_data_size;
}

struct HtmlStatusContext
{
    HFactory                     m_Factory;
    const dmHttpServer::Request* m_Request;
};

void SentHtmlStatus(HtmlStatusContext* context, const uint64_t* resource_hash, const char** file_name)
{
    SResourceDescriptor* rd = context->m_Factory->m_Resources->Get(*resource_hash);
    assert(rd);

    char tmp[32];
    DM_SNPRINTF(tmp, sizeof(tmp), "%d", rd->m_ReferenceCount);

    dmHttpServer::Send(context->m_Request, "<td>", strlen("<td>"));
    dmHttpServer::Send(context->m_Request, *file_name, strlen(*file_name));

    dmHttpServer::Send(context->m_Request, "<td>", strlen("<td>"));
    dmHttpServer::Send(context->m_Request, tmp, strlen(tmp));

    dmHttpServer::Send(context->m_Request, "<tr/>", strlen("<tr/>"));
}

void HttpServerHeader(void* user_data, const char* key, const char* value)
{
    (void) user_data;
    (void) key;
    (void) value;
}

void HttpServerResponse(void* user_data, const dmHttpServer::Request* request)
{
    SResourceFactory* factory = (SResourceFactory*) user_data;

    if (strncmp(request->m_Resource, "/reload/", strlen("/reload/")) == 0)
    {
        const char* name = request->m_Resource + strlen("/reload/");

        SResourceDescriptor* descriptor;
        ReloadResult result = ReloadResource(factory, name, &descriptor);
        switch (result)
        {
            case RELOAD_RESULT_OK:
                dmLogInfo("%s was successfully reloaded.", name);
                break;
            case RELOAD_RESULT_OUT_OF_MEMORY:
                dmLogError("Not enough memory to reload %s.", name);
                break;
            case RELOAD_RESULT_FORMAT_ERROR:
            case RELOAD_RESULT_CONSTANT_ERROR:
                dmLogError("%s has invalid format and could not be reloaded.", name);
                break;
            case RELOAD_RESULT_NOT_FOUND:
                dmLogError("%s could not be reloaded since it was never loaded before.", name);
                break;
            case RELOAD_RESULT_LOAD_ERROR:
                dmLogError("%s could not be loaded, reloading failed.", name);
                break;
            case RELOAD_RESULT_NOT_SUPPORTED:
                dmLogWarning("Reloading of resource type %s not supported.", ((SResourceType*)descriptor->m_ResourceType)->m_Extension);
                break;
            case RELOAD_RESULT_UNKNOWN:
            default:
                dmLogWarning("%s could not be reloaded, unknown error: %d.", name, result);
                break;
        }
    }
    else if(strcmp("/", request->m_Resource) == 0)
    {
        HtmlStatusContext context;
        context.m_Factory = factory;
        context.m_Request = request;
        if (factory->m_ResourceHashToFilename)
        {
            dmHttpServer::Send(request, "<table>", strlen("<table>"));

            const char* th = "<td><b>Filename</b></td><td><b>Reference count</b></td><tr/>";
            dmHttpServer::Send(request, th, strlen(th));

            factory->m_ResourceHashToFilename->Iterate(&SentHtmlStatus, &context);
            dmHttpServer::Send(request, "</table>", strlen("</table>"));
        }
    }
}

HFactory NewFactory(NewFactoryParams* params, const char* uri)
{
    // NOTE: We need an extra byte for null-termination.
    // Legacy reason (load python scripts from "const char*")
    // The gui-system still relies on this behaviour (luaL_loadstring)

    void* buffer = malloc(params->m_StreamBufferSize + 1);
    if (!buffer)
        return 0;

    SResourceFactory* factory = new SResourceFactory;

    dmURI::Result uri_result = dmURI::Parse(uri, &factory->m_UriParts);
    if (uri_result != dmURI::RESULT_OK)
    {
        dmLogError("Unable to parse uri: %s", uri);
        free(buffer);
        delete factory;
        return 0;
    }

    factory->m_HttpClient = 0;

    if (strcmp(factory->m_UriParts.m_Scheme, "http") == 0)
    {
        dmHttpClient::NewParams http_params;
        http_params.m_HttpHeader = &HttpHeader;
        http_params.m_HttpContent = &HttpContent;
        http_params.m_Userdata = factory;
        factory->m_HttpClient = dmHttpClient::New(&http_params, factory->m_UriParts.m_Hostname, factory->m_UriParts.m_Port);
        if (!factory->m_HttpClient)
        {
            dmLogError("Invalid URI: %s", uri);
            free(buffer);
            delete factory;
            return 0;
        }
    }
    else if (strcmp(factory->m_UriParts.m_Scheme, "file") == 0)
    {
        // Ok
    }
    else
    {
        dmLogError("Invalid URI: %s", uri);
        free(buffer);
        delete factory;
        return 0;
    }

    factory->m_ResourceTypesCount = 0;
    factory->m_StreamBufferSize = params->m_StreamBufferSize;
    factory->m_StreamBuffer = (char*) buffer;

    const uint32_t table_size = (3 * params->m_MaxResources) / 4;
    factory->m_Resources = new dmHashTable<uint64_t, SResourceDescriptor>();
    factory->m_Resources->SetCapacity(table_size, params->m_MaxResources);

    factory->m_ResourceToHash = new dmHashTable<uintptr_t, uint64_t>();
    factory->m_ResourceToHash->SetCapacity(table_size, params->m_MaxResources);


    factory->m_HttpServer = 0;
    if (params->m_Flags & RESOURCE_FACTORY_FLAGS_HTTP_SERVER)
    {
        // http support implies this flag implicitly
        params->m_Flags |= RESOURCE_FACTORY_FLAGS_RELOAD_SUPPORT;

        uint16_t port = 8001;
        dmHttpServer::NewParams http_server_params;
        http_server_params.m_Userdata = factory;
        http_server_params.m_HttpHeader = HttpServerHeader;
        http_server_params.m_HttpResponse = HttpServerResponse;

        dmHttpServer::Result r = dmHttpServer::New(&http_server_params, port, &factory->m_HttpServer);
        if (r != dmHttpServer::RESULT_OK)
        {
            dmLogWarning("Unable to start http server on port: %d", port);
        }
    }

    if (params->m_Flags & RESOURCE_FACTORY_FLAGS_RELOAD_SUPPORT)
    {
        factory->m_ResourceHashToFilename = new dmHashTable<uint64_t, const char*>();
        factory->m_ResourceHashToFilename->SetCapacity(table_size, params->m_MaxResources);

        factory->m_ResourceReloadedCallbacks = new dmArray<ResourceReloadedCallbackPair>();
        factory->m_ResourceReloadedCallbacks->SetCapacity(MAX_CALLBACKS);
    }
    else
    {
        factory->m_ResourceHashToFilename = 0;
        factory->m_ResourceReloadedCallbacks = 0;
    }

    if (params->m_BuiltinsArchive)
    {
        dmResourceArchive::WrapArchiveBuffer(params->m_BuiltinsArchive, params->m_BuiltinsArchiveSize, &factory->m_BuiltinsArchive);
    }
    else
    {
        factory->m_BuiltinsArchive = 0;
    }

    return factory;
}

void DeleteFactory(HFactory factory)
{
    free(factory->m_StreamBuffer);
    if (factory->m_HttpClient)
    {
        dmHttpClient::Delete(factory->m_HttpClient);
    }
    if (factory->m_HttpServer)
    {
        dmHttpServer::Delete(factory->m_HttpServer);
    }
    delete factory->m_Resources;
    delete factory->m_ResourceToHash;
    if (factory->m_ResourceHashToFilename)
        delete factory->m_ResourceHashToFilename;
    if (factory->m_ResourceReloadedCallbacks)
        delete factory->m_ResourceReloadedCallbacks;
    delete factory;
}

void UpdateFactory(HFactory factory)
{
    if (factory->m_HttpServer)
        dmHttpServer::Update(factory->m_HttpServer);
}

FactoryResult RegisterType(HFactory factory,
                           const char* extension,
                           void* context,
                           FResourceCreate create_function,
                           FResourceDestroy destroy_function,
                           FResourceRecreate recreate_function)
{
    if (factory->m_ResourceTypesCount == MAX_RESOURCE_TYPES)
        return FACTORY_RESULT_OUT_OF_RESOURCES;

    // Dots not allowed in extension
    if (strrchr(extension, '.') != 0)
        return FACTORY_RESULT_INVAL;

    if (create_function == 0 || destroy_function == 0)
        return FACTORY_RESULT_INVAL;

    if (FindResourceType(factory, extension) != 0)
        return FACTORY_RESULT_ALREADY_REGISTERED;

    SResourceType resource_type;
    resource_type.m_Extension = extension;
    resource_type.m_Context = context;
    resource_type.m_CreateFunction = create_function;
    resource_type.m_DestroyFunction = destroy_function;
    resource_type.m_RecreateFunction = recreate_function;

    factory->m_ResourceTypes[factory->m_ResourceTypesCount++] = resource_type;

    return FACTORY_RESULT_OK;
}

static FactoryResult LoadResource(HFactory factory, const char* path, const char* original_name, uint32_t* resource_size)
{
    if (factory->m_BuiltinsArchive)
    {
        dmResourceArchive::EntryInfo entry_info;
        dmResourceArchive::Result r = dmResourceArchive::FindEntry(factory->m_BuiltinsArchive, original_name, &entry_info);
        if (r == dmResourceArchive::RESULT_OK)
        {
            uint32_t file_size = entry_info.m_Size;
            // Extra byte for resources expecting null-terminated string...
            if (file_size + 1 >= factory->m_StreamBufferSize)
            {
                dmLogError("Resource too large for streambuffer: %s", path);
                return FACTORY_RESULT_STREAMBUFFER_TOO_SMALL;
            }

            memcpy(factory->m_StreamBuffer, entry_info.m_Resource, file_size);
            factory->m_StreamBuffer[file_size] = 0; // Null-terminate. See comment above
            *resource_size = file_size;

            return FACTORY_RESULT_OK;
        }
    }

    // NOTE: No else if here. Fall through
    if (factory->m_HttpClient)
    {
        // Load over HTTP
        *resource_size = 0;
        factory->m_HttpContentLength = 0;
        factory->m_HttpTotalBytesStreamed = 0;
        factory->m_HttpFactoryResult = FACTORY_RESULT_OK;
        factory->m_HttpStatus = -1;

        dmHttpClient::Result http_result = dmHttpClient::Get(factory->m_HttpClient, path);
        if (http_result != dmHttpClient::RESULT_OK)
        {
            dmLogError("Resource not found: %s", path);

            if (factory->m_HttpStatus == 404)
            {
                return FACTORY_RESULT_RESOURCE_NOT_FOUND;
            }
            else
            {
                if (http_result == dmHttpClient::RESULT_NOT_200_OK)
                    dmLogWarning("Unexpected http status code: %d", factory->m_HttpStatus);
                return FACTORY_RESULT_IO_ERROR;
            }
        }

        if (factory->m_HttpFactoryResult != FACTORY_RESULT_OK)
            return factory->m_HttpFactoryResult;

        if (factory->m_HttpContentLength != factory->m_HttpTotalBytesStreamed)
        {
            dmLogError("Expected content length differs from actually streamed for resource %s (%d != %d)", path, factory->m_HttpContentLength, factory->m_HttpTotalBytesStreamed);
        }

        // Extra byte for resources expecting null-terminated string...
        if (factory->m_HttpTotalBytesStreamed + 1 >= factory->m_StreamBufferSize)
        {
            dmLogError("Resource too large for streambuffer: %s", path);
        }
        factory->m_StreamBuffer[factory->m_HttpTotalBytesStreamed] = 0; // Null-terminate. See comment above

        *resource_size = factory->m_HttpTotalBytesStreamed;
        return FACTORY_RESULT_OK;
    }
    else
    {
        // Load over local file system

        FILE* f = fopen(path, "rb");
        if (f == 0)
        {
            dmLogError("Resource not found: %s", path);
            return FACTORY_RESULT_RESOURCE_NOT_FOUND;
        }

        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        fseek(f, 0, SEEK_SET);

        *resource_size = (uint32_t) file_size;

        // Extra byte for resources expecting null-terminated string...
        if (file_size + 1 >= (long) factory->m_StreamBufferSize)
        {
            dmLogError("Resource too large for streambuffer: %s", path);
            fclose(f);
            return FACTORY_RESULT_STREAMBUFFER_TOO_SMALL;
        }
        factory->m_StreamBuffer[file_size] = 0; // Null-terminate. See comment above

        if (fread(factory->m_StreamBuffer, 1, file_size, f) != (size_t) file_size)
        {
            fclose(f);
            return FACTORY_RESULT_IO_ERROR;
        }

        fclose(f);
        return FACTORY_RESULT_OK;
    }
}

FactoryResult Get(HFactory factory, const char* name, void** resource)
{
    assert(name);
    assert(resource);

    DM_PROFILE(Resource, "Get");

    *resource = 0;

#if 1
    char canonical_path[RESOURCE_PATH_MAX];
    GetCanonicalPath(factory->m_UriParts.m_Path, name, canonical_path);

#else
#ifdef _WIN32
    char canonical_path[_PATH_MAX];
    char *canonical_path_p = _fullpath(canonical_path, name, _PATH_MAX);
#else
    char canonical_path[PATH_MAX];
    char *canonical_path_p = realpath(name, canonical_path);
#endif

    if (canonical_path_p == 0)
    {
        return FACTORY_RESULT_RESOURCE_NOT_FOUND;
    }
#endif

    uint64_t canonical_path_hash = dmHashBuffer64(canonical_path, strlen(canonical_path));

    SResourceDescriptor* rd = factory->m_Resources->Get(canonical_path_hash);
    if (rd)
    {
        assert(factory->m_ResourceToHash->Get((uintptr_t) rd->m_Resource));
        rd->m_ReferenceCount++;
        *resource = rd->m_Resource;
        return FACTORY_RESULT_OK;
    }

    const char* ext = strrchr(name, '.');
    if (ext)
    {
        ext++;

        SResourceType* resource_type = FindResourceType(factory, ext);
        if (resource_type == 0)
        {
            dmLogError("Unknown resource type: %s", ext);
            return FACTORY_RESULT_UNKNOWN_RESOURCE_TYPE;
        }

        uint32_t file_size;
        FactoryResult result = LoadResource(factory, canonical_path, name, &file_size);
        if (result != FACTORY_RESULT_OK)
            return result;

        // TODO: We should *NOT* allocate SResource dynamically...
        SResourceDescriptor tmp_resource;
        memset(&tmp_resource, 0, sizeof(tmp_resource));
        tmp_resource.m_NameHash = canonical_path_hash;
        tmp_resource.m_ReferenceCount = 1;
        tmp_resource.m_ResourceType = (void*) resource_type;

        CreateResult create_error = resource_type->m_CreateFunction(factory, resource_type->m_Context, factory->m_StreamBuffer, file_size, &tmp_resource, name);

        if (create_error == CREATE_RESULT_OK)
        {
            assert(tmp_resource.m_Resource); // TODO: Or handle gracefully!
            factory->m_Resources->Put(canonical_path_hash, tmp_resource);
            factory->m_ResourceToHash->Put((uintptr_t) tmp_resource.m_Resource, canonical_path_hash);
            if (factory->m_ResourceHashToFilename)
            {
                factory->m_ResourceHashToFilename->Put(canonical_path_hash, strdup(canonical_path));
            }
            *resource = tmp_resource.m_Resource;
            return FACTORY_RESULT_OK;
        }
        else
        {
            dmLogWarning("Unable to create resource: %s", canonical_path);
            // TODO: Map CreateResult to FactoryResult here.
            return FACTORY_RESULT_UNKNOWN;
        }
    }
    else
    {
        dmLogWarning("Unable to load resource: '%s'. Missing file extension.", name);
        return FACTORY_RESULT_MISSING_FILE_EXTENSION;
    }
}

ReloadResult ReloadResource(HFactory factory, const char* name, SResourceDescriptor** out_descriptor)
{
    char canonical_path[RESOURCE_PATH_MAX];
    GetCanonicalPath(factory->m_UriParts.m_Path, name, canonical_path);

    uint64_t canonical_path_hash = dmHashBuffer64(canonical_path, strlen(canonical_path));

    SResourceDescriptor* rd = factory->m_Resources->Get(canonical_path_hash);

    if (out_descriptor)
        *out_descriptor = rd;

    if (rd == 0x0)
        return RELOAD_RESULT_NOT_FOUND;

    SResourceType* resource_type = (SResourceType*) rd->m_ResourceType;
    if (!resource_type->m_RecreateFunction)
        return RELOAD_RESULT_NOT_SUPPORTED;

    uint32_t file_size;
    FactoryResult result = LoadResource(factory, canonical_path, name, &file_size);
    if (result != FACTORY_RESULT_OK)
        return RELOAD_RESULT_LOAD_ERROR;

    CreateResult create_result = resource_type->m_RecreateFunction(factory, resource_type->m_Context, factory->m_StreamBuffer, file_size, rd, name);
    if (create_result == CREATE_RESULT_OK)
    {
        if (factory->m_ResourceReloadedCallbacks)
        {
            for (uint32_t i = 0; i < factory->m_ResourceReloadedCallbacks->Size(); ++i)
            {
                ResourceReloadedCallbackPair& pair = (*factory->m_ResourceReloadedCallbacks)[i];
                pair.m_Callback(pair.m_UserData, rd, name);
            }
        }
        return RELOAD_RESULT_OK;
    }
    else
    {
        switch (create_result)
        {
            case CREATE_RESULT_OUT_OF_MEMORY:   return RELOAD_RESULT_OUT_OF_MEMORY;
            case CREATE_RESULT_FORMAT_ERROR:    return RELOAD_RESULT_FORMAT_ERROR;
            case CREATE_RESULT_CONSTANT_ERROR:  return RELOAD_RESULT_CONSTANT_ERROR;
            case CREATE_RESULT_UNKNOWN:         return RELOAD_RESULT_UNKNOWN;
            default:                            return RELOAD_RESULT_UNKNOWN;
        }
    }
}

FactoryResult GetType(HFactory factory, void* resource, uint32_t* type)
{
    assert(type);

    uint64_t* resource_hash = factory->m_ResourceToHash->Get((uintptr_t) resource);
    if (!resource_hash)
    {
        return FACTORY_RESULT_NOT_LOADED;
    }

    SResourceDescriptor* rd = factory->m_Resources->Get(*resource_hash);
    assert(rd);
    assert(rd->m_ReferenceCount > 0);
    *type = (uint32_t) rd->m_ResourceType; // TODO: Not 64-bit friendly...

    return FACTORY_RESULT_OK;
}

FactoryResult GetTypeFromExtension(HFactory factory, const char* extension, uint32_t* type)
{
    assert(type);

    SResourceType* resource_type = FindResourceType(factory, extension);
    if (resource_type)
    {
        *type = (uint32_t) resource_type; // TODO: Not 64-bit friendly...
        return FACTORY_RESULT_OK;
    }
    else
    {
        return FACTORY_RESULT_UNKNOWN_RESOURCE_TYPE;
    }
}

FactoryResult GetExtensionFromType(HFactory factory, uint32_t type, const char** extension)
{
    for (uint32_t i = 0; i < factory->m_ResourceTypesCount; ++i)
    {
        SResourceType* rt = &factory->m_ResourceTypes[i];

        if (((uintptr_t) rt) == type)
        {
            *extension = rt->m_Extension;
            return FACTORY_RESULT_OK;
        }
    }

    *extension = 0;
    return FACTORY_RESULT_UNKNOWN_RESOURCE_TYPE;
}

FactoryResult GetDescriptor(HFactory factory, const char* name, SResourceDescriptor* descriptor)
{
    char canonical_path[RESOURCE_PATH_MAX];
    GetCanonicalPath(factory->m_UriParts.m_Path, name, canonical_path);

    uint64_t canonical_path_hash = dmHashBuffer64(canonical_path, strlen(canonical_path));

    SResourceDescriptor* tmp_descriptor = factory->m_Resources->Get(canonical_path_hash);
    if (tmp_descriptor)
    {
        *descriptor = *tmp_descriptor;
        return FACTORY_RESULT_OK;
    }
    else
    {
        return FACTORY_RESULT_NOT_LOADED;
    }
}

void Release(HFactory factory, void* resource)
{
    uint64_t* resource_hash = factory->m_ResourceToHash->Get((uintptr_t) resource);
    assert(resource_hash);

    SResourceDescriptor* rd = factory->m_Resources->Get(*resource_hash);
    assert(rd);
    assert(rd->m_ReferenceCount > 0);
    rd->m_ReferenceCount--;

    if (rd->m_ReferenceCount == 0)
    {
        SResourceType* resource_type = (SResourceType*) rd->m_ResourceType;
        resource_type->m_DestroyFunction(factory, resource_type->m_Context, rd);

        factory->m_ResourceToHash->Erase((uintptr_t) resource);
        factory->m_Resources->Erase(*resource_hash);
        if (factory->m_ResourceHashToFilename)
        {
            const char** s = factory->m_ResourceHashToFilename->Get(*resource_hash);
            factory->m_ResourceHashToFilename->Erase(*resource_hash);
            assert(s);
            free((void*) *s);
        }
    }
}

void RegisterResourceReloadedCallback(HFactory factory, ResourceReloadedCallback callback, void* user_data)
{
    if (factory->m_ResourceReloadedCallbacks)
    {
        if (!factory->m_ResourceReloadedCallbacks->Full())
        {
            ResourceReloadedCallbackPair pair;
            pair.m_Callback = callback;
            pair.m_UserData = user_data;
            factory->m_ResourceReloadedCallbacks->Push(pair);
        }
        else
        {
            dmLogWarning("Resource reloaded callback could not be registered since the maximum capacity of callbacks has been reached (%d).", MAX_CALLBACKS);
        }
    }
}

void UnregisterResourceReloadedCallback(HFactory factory, ResourceReloadedCallback callback, void* user_data)
{
    if (factory->m_ResourceReloadedCallbacks)
    {
        uint32_t i = 0;
        uint32_t size = factory->m_ResourceReloadedCallbacks->Size();
        while (i < size)
        {
            ResourceReloadedCallbackPair& pair = (*factory->m_ResourceReloadedCallbacks)[i];
            if (pair.m_Callback == callback && pair.m_UserData == user_data)
            {
                factory->m_ResourceReloadedCallbacks->EraseSwap(i);
                --size;
            }
            else
            {
                ++i;
            }
        }
    }
}

}

