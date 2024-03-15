//
//  platform.h
//  ccpUtils
//
//  Created by Kristjan Valur Jonsson on 17/03/14.
//
//

#ifndef ccputils_platform_h
#define ccputils_platform_h

#ifdef _WIN32
#include "BluePlatformWin32.h"
#else
#include "BluePlatformPosix.h"
#endif

namespace Ccp {
    
    // A template based Acquirer for any class supporting Acquire and Release
    template<class T>
    class Acquirer
    {
    public:
        Acquirer( T &t) :
        mTarget(t), mReleased(false)
        {
            mTarget.Acquire();
        }
        
        ~Acquirer() throw()
        {
            // Must't throw exceptions here.
            try {
                Release();
            } catch (const std::exception &e) {
                ReportUnraisable(e);
            } catch (...) {
                ReportUnraisable();
            }
        }
        
        void Release()
        {
            if (!mReleased) {
                mReleased = true;
                mTarget.Release();
            }
        }
        
    private:
        Acquirer(const Acquirer &);
        Acquirer &operator= (const Acquirer &);
        
    private:
        T &mTarget;
        bool mReleased;
    };
    
    // Similarly a releaser
    template<typename T>
    class Releaser
    {
    public:
        Releaser( T &t) :
        mTarget(t)
        {
            mTarget.Release();
        }
        
        ~Releaser() throw()
        {
            // Must't throw exceptions here.
            try {
                mTarget.Acquire();
            } catch (const std::exception &e) {
                ReportUnraisable(e);
            } catch (...) {
                ReportUnraisable();
            }
        }
        
    private:
        Releaser(const Releaser &);
        Releaser &operator= (const Releaser &);
        
    private:
        T &mTarget;
    };
    

    
    // A RAII class to manage critical section ownership
    typedef Acquirer<Mutex> MutexOwner;
    typedef Releaser<Mutex> MutexReleaser;
    typedef MutexOwner CriticalSectionOwner; // historical
    
    
    //A singlestore container template, to automatically release resources.
    //Useful to make sure that HANDLEs are released
    template <class T>
    class Singlestore : public T
    {
        typedef typename T::_type _type;
    public:
        explicit Singlestore(_type _s = T::Invalid()) : mS(_s) {}
        Singlestore(Singlestore &o) : mS(T::Invalid()) {Swap(o);}
        ~Singlestore() throw() {
            try {
                Close();
            } catch (const std::exception &e) {
                ReportUnraisable(e);
            } catch (...) {
                ReportUnraisable();
            }
        }
        
        void Close() {
            if (Valid())
                T::Close(mS);
            mS = T::Invalid();
        }
        bool Valid() const {return !T::IsInvalid(mS);}
        void Swap(Singlestore &o) {
            _type tmp = mS;
            mS = o.mS;
            o.mS = tmp;
        }
        _type Peek() const {return mS;}
        _type Detach() {
            _type tmp = mS;
            mS = T::Invalid();
            return tmp;
        }
    private:
        Singlestore &operator =(const Singlestore &o); //no assignment
        _type mS;
    };
    
    typedef Singlestore<_Handle> Handle;

    
}; // namespace


#endif