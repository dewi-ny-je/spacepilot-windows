// Per-user credentials for the 3DconnexionJS-compatible WebSocket endpoint.
// Windows already routes all of 127.0.0.0/8 to loopback, so unlike macOS no
// address alias or administrator access is needed. The CA is trusted only in
// the current user's root store and is name-constrained to 127.51.68.120.
#include "axial/platform.hpp"
#include <wincrypt.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509v3.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs=std::filesystem;
namespace {
constexpr const char* address="127.51.68.120";
fs::path directory(){return fs::path(sn::wide(sn::dataDirectory()))/L"Web";}
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
using Key=std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)>;
using Certificate=std::unique_ptr<X509,decltype(&X509_free)>;
Key key(){Key result(EVP_PKEY_Q_keygen(nullptr,nullptr,"RSA",size_t(3072)),EVP_PKEY_free);require(bool(result),"Key generation failed");return result;}
Certificate certificate(EVP_PKEY* key,const char* commonName,long days){
    Certificate cert(X509_new(),X509_free);require(bool(cert),"Certificate allocation failed");
    require(X509_set_version(cert.get(),2)==1,"Certificate version failed");
    unsigned char serial[16];require(RAND_bytes(serial,sizeof(serial))==1,"Serial generation failed");serial[0]&=0x7f;
    BIGNUM* bn=BN_bin2bn(serial,sizeof(serial),nullptr);require(bn!=nullptr,"Serial allocation failed");
    ASN1_INTEGER* value=BN_to_ASN1_INTEGER(bn,nullptr);BN_free(bn);require(value!=nullptr,"Serial encoding failed");
    int result=X509_set_serialNumber(cert.get(),value);ASN1_INTEGER_free(value);require(result==1,"Serial assignment failed");
    require(X509_gmtime_adj(X509_getm_notBefore(cert.get()),-300)!=nullptr&&X509_gmtime_adj(X509_getm_notAfter(cert.get()),days*24*3600)!=nullptr,"Certificate validity failed");
    require(X509_set_pubkey(cert.get(),key)==1,"Certificate public key failed");
    require(X509_NAME_add_entry_by_txt(X509_get_subject_name(cert.get()),"CN",MBSTRING_ASC,reinterpret_cast<const unsigned char*>(commonName),-1,-1,0)==1,"Certificate subject failed");
    return cert;
}
void extension(X509* cert,X509* issuer,int nid,const char* value){
    X509V3_CTX context;X509V3_set_ctx(&context,issuer,cert,nullptr,nullptr,0);
    X509_EXTENSION* ext=X509V3_EXT_conf_nid(nullptr,&context,nid,value);require(ext!=nullptr,"Certificate extension failed");
    int result=X509_add_ext(cert,ext,-1);X509_EXTENSION_free(ext);require(result==1,"Certificate extension assignment failed");
}
template<class Writer> void write(const fs::path& path,Writer writer){
    // CREATE_NEW never replaces existing credentials or follows a planted file.
    HANDLE handle=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(handle!=INVALID_HANDLE_VALUE,"Credential already exists or cannot be written");
    BIO* memory=BIO_new(BIO_s_mem());bool ok=memory&&writer(memory)==1;
    char* data=nullptr;long length=memory?BIO_get_mem_data(memory,&data):0;DWORD written=0;
    ok=ok&&WriteFile(handle,data,DWORD(length),&written,nullptr)&&written==DWORD(length)&&FlushFileBuffers(handle);
    if(memory)BIO_free(memory);CloseHandle(handle);
    if(!ok){DeleteFileW(path.c_str());throw std::runtime_error("Cannot save credentials");}
}
std::string uuid(){std::random_device random;char text[40];snprintf(text,sizeof(text),"%08X-%04X-%04X",random(),random()&0xffff,random()&0xffff);return text;}
void prepare(const fs::path& target){
    std::error_code ec;fs::create_directories(target,ec);
    require(fs::is_directory(target)&&!fs::is_symlink(target),"Cannot create credential directory");
    for(const wchar_t* name:{L"root.crt",L"server.crt",L"server.key"})require(!fs::exists(fs::symlink_status(target/name)),"Credentials already exist");
    auto rootKey=key(),serverKey=key();
    auto root=certificate(rootKey.get(),("Axial Local Web CA "+uuid()).c_str(),3650);
    X509_set_issuer_name(root.get(),X509_get_subject_name(root.get()));
    extension(root.get(),root.get(),NID_basic_constraints,"critical,CA:TRUE,pathlen:0");
    extension(root.get(),root.get(),NID_key_usage,"critical,keyCertSign,cRLSign");
    extension(root.get(),root.get(),NID_subject_key_identifier,"hash");
    // Even if the CA were misused, Windows only accepts it for this loopback address.
    extension(root.get(),root.get(),NID_name_constraints,"critical,permitted;IP:127.51.68.120/255.255.255.255");
    require(X509_sign(root.get(),rootKey.get(),EVP_sha256())>0,"CA signing failed");
    // Browsers cap privately issued TLS leaf validity at 825 days.
    auto server=certificate(serverKey.get(),"Axial loopback",730);X509_set_issuer_name(server.get(),X509_get_subject_name(root.get()));
    extension(server.get(),root.get(),NID_basic_constraints,"critical,CA:FALSE");
    extension(server.get(),root.get(),NID_key_usage,"critical,digitalSignature,keyEncipherment");
    extension(server.get(),root.get(),NID_ext_key_usage,"serverAuth");
    extension(server.get(),root.get(),NID_subject_alt_name,"IP:127.51.68.120");
    extension(server.get(),root.get(),NID_authority_key_identifier,"keyid:always");
    require(X509_sign(server.get(),rootKey.get(),EVP_sha256())>0,"Server signing failed");
    write(target/L"server.key",[&](BIO* b){return PEM_write_bio_PrivateKey(b,serverKey.get(),nullptr,nullptr,0,nullptr,nullptr);});
    write(target/L"server.crt",[&](BIO* b){return PEM_write_bio_X509(b,server.get());});
    write(target/L"root.crt",[&](BIO* b){return PEM_write_bio_X509(b,root.get());});
    // The CA private key never leaves memory and is freed here.
}
Certificate readCertificate(const fs::path& file){
    FILE* f=_wfopen(file.c_str(),L"rb");if(!f)return {nullptr,X509_free};
    Certificate cert(PEM_read_X509(f,nullptr,nullptr,nullptr),X509_free);fclose(f);return cert;
}
bool validCredentials(const fs::path& target){
    auto root=readCertificate(target/L"root.crt"),server=readCertificate(target/L"server.crt");
    time_t renewal=time(nullptr)+90L*24*3600;
    if(!root||!server||X509_cmp_time(X509_get0_notAfter(root.get()),&renewal)<=0||X509_cmp_time(X509_get0_notAfter(server.get()),&renewal)<=0||X509_check_ip_asc(server.get(),address,0)!=1)return false;
    int days=0,seconds=0;
    if(X509_cmp_current_time(X509_get0_notBefore(server.get()))>=0||ASN1_TIME_diff(&days,&seconds,X509_get0_notBefore(server.get()),X509_get0_notAfter(server.get()))!=1||days>825||(days==825&&seconds>0))return false;
    Key publicKey(X509_get_pubkey(root.get()),EVP_PKEY_free);if(!publicKey||X509_verify(server.get(),publicKey.get())!=1)return false;
    FILE* file=_wfopen((target/L"server.key").c_str(),L"rb");if(!file)return false;
    Key privateKey(PEM_read_PrivateKey(file,nullptr,nullptr,nullptr),EVP_PKEY_free);fclose(file);
    return privateKey&&X509_check_private_key(server.get(),privateKey.get())==1;
}
std::vector<unsigned char> der(X509* cert){
    int length=i2d_X509(cert,nullptr);require(length>0,"Cannot encode certificate");
    std::vector<unsigned char> bytes(size_t(length));auto output=bytes.data();require(i2d_X509(cert,&output)==length,"Cannot encode certificate");
    return bytes;
}
HCERTSTORE userRoot(){return CertOpenStore(CERT_STORE_PROV_SYSTEM_W,0,0,CERT_SYSTEM_STORE_CURRENT_USER,L"Root");}
// Evaluate the server certificate exactly as Windows TLS clients do, without
// supplying our CA: only an installed, trusted root makes this succeed.
bool trusted(const fs::path& target){
    auto server=readCertificate(target/L"server.crt");if(!server)return false;
    auto bytes=der(server.get());
    PCCERT_CONTEXT leaf=CertCreateCertificateContext(X509_ASN_ENCODING,bytes.data(),DWORD(bytes.size()));if(!leaf)return false;
    CERT_CHAIN_PARA parameters{};parameters.cbSize=sizeof(parameters);
    LPSTR usage=const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH);
    parameters.RequestedUsage.dwType=USAGE_MATCH_TYPE_AND;parameters.RequestedUsage.Usage.cUsageIdentifier=1;parameters.RequestedUsage.Usage.rgpszUsageIdentifier=&usage;
    PCCERT_CHAIN_CONTEXT chain=nullptr;bool ok=false;
    if(CertGetCertificateChain(nullptr,leaf,nullptr,nullptr,&parameters,CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL,nullptr,&chain)){
        std::wstring host=L"127.51.68.120";
        SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl{};ssl.cbSize=sizeof(ssl);ssl.dwAuthType=AUTHTYPE_SERVER;ssl.pwszServerName=host.data();
        CERT_CHAIN_POLICY_PARA policy{};policy.cbSize=sizeof(policy);policy.pvExtraPolicyPara=&ssl;
        CERT_CHAIN_POLICY_STATUS status{};status.cbSize=sizeof(status);
        ok=CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL,chain,&policy,&status)&&status.dwError==0;
        CertFreeCertificateChain(chain);
    }
    CertFreeCertificateContext(leaf);return ok;
}
void untrust(const fs::path& target){
    auto root=readCertificate(target/L"root.crt");if(!root)return;
    auto bytes=der(root.get());HCERTSTORE store=userRoot();if(!store)return;
    PCCERT_CONTEXT wanted=CertCreateCertificateContext(X509_ASN_ENCODING,bytes.data(),DWORD(bytes.size()));
    if(wanted){
        // Removing a root from the user store shows a Windows confirmation.
        if(PCCERT_CONTEXT found=CertFindCertificateInStore(store,X509_ASN_ENCODING,0,CERT_FIND_EXISTING,wanted,nullptr))CertDeleteCertificateFromStore(found);
        CertFreeCertificateContext(wanted);
    }
    CertCloseStore(store,0);
}
void trust(const fs::path& target){
    auto root=readCertificate(target/L"root.crt");require(bool(root),"Cannot read web CA");
    auto bytes=der(root.get());HCERTSTORE store=userRoot();require(store!=nullptr,"Cannot open the certificate store");
    // Windows asks the user to approve a new trusted root; cancelling fails here.
    BOOL added=CertAddEncodedCertificateToStore(store,X509_ASN_ENCODING,bytes.data(),DWORD(bytes.size()),CERT_STORE_ADD_USE_EXISTING,nullptr);
    DWORD error=GetLastError();CertCloseStore(store,0);
    if(!added&&(error==ERROR_CANCELLED||error==HRESULT_FROM_WIN32(ERROR_CANCELLED)))throw std::runtime_error("cancelled");
    require(added,"Cannot store the local web certificate");
}
void install(){
    auto target=directory();std::error_code ec;fs::create_directories(target,ec);
    require(fs::is_directory(target)&&!fs::is_symlink(target),"Cannot create credential directory");
    if(!validCredentials(target)){
        auto staging=target/sn::wide("generate-"+uuid());
        try{
            prepare(staging);untrust(target);
            for(const wchar_t* file:{L"root.crt",L"server.crt",L"server.key"})
                require(MoveFileExW((staging/file).c_str(),(target/file).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH),"Cannot install credentials");
        }catch(...){fs::remove_all(staging,ec);throw;}
        fs::remove_all(staging,ec);
    }
    if(!trusted(target))trust(target);
}
void uninstall(){
    auto target=directory();if(!fs::exists(target))return;
    untrust(target);
    std::error_code ec;for(const wchar_t* name:{L"root.crt",L"server.crt",L"server.key"})fs::remove(target/name,ec);
    fs::remove(target,ec);
}
}
int wmain(int argc,wchar_t** argv){try{
    OPENSSL_init_crypto(OPENSSL_INIT_NO_LOAD_CONFIG,nullptr);
    std::wstring command=argc>1?argv[1]:L"";
    if(argc==3&&command==L"--prepare"){prepare(argv[2]);return 0;}
    if(argc==2&&command==L"--uninstall"){uninstall();return 0;}
    if(argc==2&&command==L"--install"){install();return 0;}
    if(argc==2&&command==L"--check"){
        auto target=directory();bool ready=validCredentials(target);
        printf("{\"credentialsReady\":%s,\"trusted\":%s}\n",ready?"true":"false",ready&&trusted(target)?"true":"false");return 0;
    }
    fprintf(stderr,"usage: axial-web-setup --install|--uninstall|--check|--prepare directory\n");return 2;
}catch(const std::exception& error){
    if(std::string(error.what())=="cancelled"){fprintf(stderr,"Axial web setup: cancelled\n");return 3;}
    fprintf(stderr,"Axial web setup: %s\n",error.what());return 1;
}}
