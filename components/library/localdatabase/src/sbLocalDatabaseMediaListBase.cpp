/*
//
// BEGIN SONGBIRD GPL
//
// This file is part of the Songbird web player.
//
// Copyright(c) 2005-2007 POTI, Inc.
// http://songbirdnest.com
//
// This file may be licensed under the terms of of the
// GNU General Public License Version 2 (the "GPL").
//
// Software distributed under the License is distributed
// on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, either
// express or implied. See the GPL for the specific language
// governing rights and limitations.
//
// You should have received a copy of the GPL along with this
// program. If not, go to http://www.gnu.org/licenses/gpl.html
// or write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
//
// END SONGBIRD GPL
//
*/

#include "sbLocalDatabaseMediaListBase.h"

#include <nsAutoLock.h>
#include <nsComponentManagerUtils.h>
#include <nsIProgrammingLanguage.h>
#include <nsIProperty.h>
#include <nsIPropertyBag.h>
#include <nsISimpleEnumerator.h>
#include <nsIStringEnumerator.h>
#include <nsIURI.h>
#include <nsIVariant.h>
#include <nsMemory.h>
#include <sbSQLBuilderCID.h>
#include <sbIDatabaseQuery.h>
#include <DatabaseQuery.h>

#include <sbIMediaList.h>
#include <sbIPropertyArray.h>
#include <sbPropertiesCID.h>
#include "sbLocalDatabaseCID.h"
#include "sbLocalDatabasePropertyCache.h"
#include <sbLocalDatabaseCascadeFilterSet.h>

NS_IMPL_ISUPPORTS_INHERITED7(sbLocalDatabaseMediaListBase,
                             sbLocalDatabaseResourceProperty,
                             sbIMediaItem,
                             sbILocalDatabaseMediaItem,
                             sbIFilterableMediaList,
                             sbISearchableMediaList,
                             sbISortableMediaList,
                             sbIMediaList,
                             nsIClassInfo)

NS_IMPL_CI_INTERFACE_GETTER7(sbLocalDatabaseMediaListBase,
                             sbILibraryResource,
                             sbIMediaItem,
                             sbILocalDatabaseMediaItem,
                             sbIFilterableMediaList,
                             sbISearchableMediaList,
                             sbISortableMediaList,
                             sbIMediaList)

sbLocalDatabaseMediaListBase::sbLocalDatabaseMediaListBase(sbILocalDatabaseLibrary* aLibrary,
                                                           const nsAString& aGuid)
: sbLocalDatabaseResourceProperty(aLibrary, aGuid),
  mLibrary(aLibrary),
  mMediaItemId(0),
  mLockedEnumerationActive(PR_FALSE)
{
  mFullArrayMonitor = nsAutoMonitor::NewMonitor("sbLocalDatabaseMediaListBase::mFullArrayMonitor");
  NS_ASSERTION(mFullArrayMonitor, "Failed to create monitor!");

  mViewArrayLock = nsAutoLock::NewLock("sbLocalDatabaseMediaListBase::mViewArrayLock");
  NS_ASSERTION(mViewArrayLock, "Failed to create lock!");

  PRBool success = mViewFilters.Init();
  NS_ASSERTION(success, "Failed to init view filter table");

  nsresult rv = sbLocalDatabaseMediaListListener::Init();
  NS_ASSERTION(NS_SUCCEEDED(rv), "Failed to initialize base listener");
}

sbLocalDatabaseMediaListBase::~sbLocalDatabaseMediaListBase()
{
  if (mFullArrayMonitor) {
    nsAutoMonitor::DestroyMonitor(mFullArrayMonitor);
  }

  if (mViewArrayLock) {
    nsAutoLock::DestroyLock(mViewArrayLock);
  }
}

/**
 * \brief Adds multiple filters to a GUID array.
 *
 * This method enumerates a hash table and calls AddFilter on a GUIDArray once 
 * for each key. It constructs a string enumerator for the string array that
 * the hash table contains.
 *
 * This method expects to be handed an sbILocalDatabaseGUIDArray pointer as its
 * aUserData parameter.
 */
/* static */ PLDHashOperator PR_CALLBACK
sbLocalDatabaseMediaListBase::AddFilterToGUIDArrayCallback(nsStringHashKey::KeyType aKey,
                                                           sbStringArray* aEntry,
                                                           void* aUserData)
{
  NS_ASSERTION(aEntry, "Null entry in the hash?!");
  NS_ASSERTION(aUserData, "Null userData!");
  
  // Make a string enumerator for the string array.
  nsCOMPtr<nsIStringEnumerator> valueEnum =
    new sbTArrayStringEnumerator(aEntry);
  
  // If we failed then we're probably out of memory. Hope we do better on the
  // next key?
  NS_ENSURE_TRUE(valueEnum, PL_DHASH_NEXT);

  // Unbox the guidArray.
  nsCOMPtr<sbILocalDatabaseGUIDArray> guidArray =
    NS_STATIC_CAST(sbILocalDatabaseGUIDArray*, aUserData);
  
  // Set the filter.
  nsresult rv = guidArray->AddFilter(aKey, valueEnum, PR_FALSE);
  NS_WARN_IF_FALSE(NS_SUCCEEDED(rv), "AddFilter failed!");

  return PL_DHASH_NEXT;
}

/**
 * Internal method that may be inside the monitor.
 */
nsresult
sbLocalDatabaseMediaListBase::EnumerateAllItemsInternal(sbIMediaListEnumerationListener* aEnumerationListener)
{
  sbGUIDArrayEnumerator enumerator(mLibrary, mFullArray);
  return EnumerateItemsInternal(&enumerator, aEnumerationListener);
}

/**
 * Internal method that may be inside the monitor.
 */
nsresult
sbLocalDatabaseMediaListBase::EnumerateItemsByPropertyInternal(const nsAString& aName,
                                                               nsIStringEnumerator* aValueEnum,
                                                               sbIMediaListEnumerationListener* aEnumerationListener)
{
  // Make a new GUID array to talk to the database.
  nsCOMPtr<sbILocalDatabaseGUIDArray> guidArray;
  nsresult rv = mFullArray->Clone(getter_AddRefs(guidArray));
  NS_ENSURE_SUCCESS(rv, rv);

  // Clone copies the filters... which we don't want.
  rv = guidArray->ClearFilters();
  NS_ENSURE_SUCCESS(rv, rv);

  // Set the filter.
  rv = guidArray->AddFilter(aName, aValueEnum, PR_FALSE);
  NS_ENSURE_SUCCESS(rv, rv);

  // And make an enumerator to return the filtered items.
  sbGUIDArrayEnumerator enumerator(mLibrary, guidArray);

  return EnumerateItemsInternal(&enumerator, aEnumerationListener);
}

/**
 * Internal method that may be inside the monitor.
 */
nsresult
sbLocalDatabaseMediaListBase::EnumerateItemsByPropertiesInternal(sbStringArrayHash* aPropertiesHash,
                                                                 sbIMediaListEnumerationListener* aEnumerationListener)
{
  nsCOMPtr<sbILocalDatabaseGUIDArray> guidArray;
  nsresult rv = mFullArray->Clone(getter_AddRefs(guidArray));
  NS_ENSURE_SUCCESS(rv, rv);

  // Clone copies the filters... which we don't want.
  rv = guidArray->ClearFilters();
  NS_ENSURE_SUCCESS(rv, rv);

  // Now that our hash table is set up we call AddFilter for each property
  // name and all its associated values.
  PRUint32 filterCount =
    aPropertiesHash->EnumerateRead(AddFilterToGUIDArrayCallback, guidArray);
  
  // Make sure we actually added some filters here. Otherwise something went
  // wrong and the results are not going to be what the caller expects.
  PRUint32 hashCount = aPropertiesHash->Count();
  NS_ENSURE_TRUE(filterCount == hashCount, NS_ERROR_UNEXPECTED);

  // Finally make an enumerator to return the filtered items.
  sbGUIDArrayEnumerator enumerator(mLibrary, guidArray);
  return EnumerateItemsInternal(&enumerator, aEnumerationListener);
}

nsresult
sbLocalDatabaseMediaListBase::MakeStandardQuery(sbIDatabaseQuery** _retval)
{
  nsresult rv;
  nsCOMPtr<sbIDatabaseQuery> query =
    do_CreateInstance(SONGBIRD_DATABASEQUERY_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  nsAutoString databaseGuid;
  rv = mLibrary->GetDatabaseGuid(databaseGuid);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = query->SetDatabaseGUID(databaseGuid);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIURI> databaseLocation;
  rv = mLibrary->GetDatabaseLocation(getter_AddRefs(databaseLocation));
  NS_ENSURE_SUCCESS(rv, rv);

  if (databaseLocation) {
    rv = query->SetDatabaseLocation(databaseLocation);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  rv = query->SetAsyncQuery(PR_FALSE);
  NS_ENSURE_SUCCESS(rv, rv);

  NS_ADDREF(*_retval = query);
  return NS_OK;
}

/**
 * \brief Enumerates the items to the given listener.
 */
nsresult
sbLocalDatabaseMediaListBase::EnumerateItemsInternal(sbGUIDArrayEnumerator* aEnumerator,
                                                     sbIMediaListEnumerationListener* aListener)
{
  // Loop until we explicitly return.
  while (PR_TRUE) {

    PRBool hasMore;
    nsresult rv = aEnumerator->HasMoreElements(&hasMore);
    NS_ENSURE_SUCCESS(rv, rv);

    if (!hasMore) {
      return NS_OK;
    }
    
    nsCOMPtr<sbIMediaItem> item;
    rv = aEnumerator->GetNext(getter_AddRefs(item));
    NS_ENSURE_SUCCESS(rv, rv);

    PRBool continueEnumerating;
    rv = aListener->OnEnumeratedItem(this, item, &continueEnumerating);
    NS_ENSURE_SUCCESS(rv, rv);

    // Stop enumerating if the listener requested it.
    if (NS_SUCCEEDED(rv) && !continueEnumerating) {
      return NS_ERROR_ABORT;
    }
  }

  NS_NOTREACHED("Uh, how'd we get here?");
  return NS_ERROR_UNEXPECTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetName(nsAString& aName)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::SetName(const nsAString& aName)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetLength(PRUint32* aLength)
{
  NS_ENSURE_ARG_POINTER(aLength);

  NS_ENSURE_TRUE(mFullArrayMonitor, NS_ERROR_FAILURE);
  nsAutoMonitor mon(mFullArrayMonitor);

  nsresult rv = mFullArray->GetLength(aLength);
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetFilteredLength(PRUint32* aLength)
{
  NS_ENSURE_ARG_POINTER(aLength);

  NS_ENSURE_TRUE(mFullArrayMonitor, NS_ERROR_FAILURE);
  nsAutoMonitor mon(mFullArrayMonitor);

  nsresult rv = mViewArray->GetLength(aLength);
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetItemByGuid(const nsAString& aGuid,
                                            sbIMediaItem** _retval)
{
  NS_ENSURE_ARG_POINTER(_retval);

  nsresult rv;
  nsCOMPtr<sbILibrary> library = do_QueryInterface(mLibrary, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<sbIMediaItem> item;
  rv = library->GetMediaItem(aGuid, getter_AddRefs(item));
  NS_ENSURE_SUCCESS(rv, rv);

  NS_ADDREF(*_retval = item);
  return NS_OK;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetItemByIndex(PRUint32 aIndex,
                                             sbIMediaItem** _retval)
{
  NS_ENSURE_ARG_POINTER(_retval);

  nsresult rv;

  nsAutoString guid;
  {
    NS_ENSURE_TRUE(mFullArrayMonitor, NS_ERROR_FAILURE);
    nsAutoMonitor mon(mFullArrayMonitor);

    rv = mFullArray->GetByIndex(aIndex, guid);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  nsCOMPtr<sbILibrary> library = do_QueryInterface(mLibrary, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<sbIMediaItem> item;
  rv = library->GetMediaItem(guid, getter_AddRefs(item));
  NS_ENSURE_SUCCESS(rv, rv);

  NS_ADDREF(*_retval = item);
  return NS_OK;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetItemByFilteredIndex(PRUint32 aIndex,
                                                     sbIMediaItem** _retval)
{
  NS_ENSURE_ARG_POINTER(_retval);

  nsresult rv;

  nsAutoString guid;
  {
    NS_ENSURE_TRUE(mFullArrayMonitor, NS_ERROR_FAILURE);
    nsAutoMonitor mon(mFullArrayMonitor);

    rv = mViewArray->GetByIndex(aIndex, guid);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  nsCOMPtr<sbILibrary> library = do_QueryInterface(mLibrary, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<sbIMediaItem> item;
  rv = library->GetMediaItem(guid, getter_AddRefs(item));
  NS_ENSURE_SUCCESS(rv, rv);

  NS_ADDREF(*_retval = item);
  return NS_OK;
}

/**
 * See sbIMediaList
 */
NS_IMETHODIMP
sbLocalDatabaseMediaListBase::EnumerateAllItems(sbIMediaListEnumerationListener* aEnumerationListener,
                                                PRUint16 aEnumerationType)
{
  NS_ENSURE_ARG_POINTER(aEnumerationListener);

  nsresult rv;

  switch (aEnumerationType) {

    case sbIMediaList::ENUMERATIONTYPE_LOCKING: {
      NS_ENSURE_TRUE(mFullArrayMonitor, NS_ERROR_FAILURE);
      nsAutoMonitor mon(mFullArrayMonitor);

      // Don't reenter!
      NS_ENSURE_FALSE(mLockedEnumerationActive, NS_ERROR_FAILURE);
      mLockedEnumerationActive = PR_TRUE;

      PRBool beginEnumeration;
      rv = aEnumerationListener->OnEnumerationBegin(this, &beginEnumeration);

      if (NS_SUCCEEDED(rv)) {
        if (beginEnumeration) {
          rv = EnumerateAllItemsInternal(aEnumerationListener);
        }
        else {
          // The user cancelled the enumeration.
          rv = NS_ERROR_ABORT;
        }
      }

      mLockedEnumerationActive = PR_FALSE;

    } break; // ENUMERATIONTYPE_LOCKING

    case sbIMediaList::ENUMERATIONTYPE_SNAPSHOT: {
      PRBool beginEnumeration;
      rv = aEnumerationListener->OnEnumerationBegin(this, &beginEnumeration);

      if (NS_SUCCEEDED(rv)) {
        if (beginEnumeration) {
          rv = EnumerateAllItemsInternal(aEnumerationListener);
        }
        else {
          // The user cancelled the enumeration.
          rv = NS_ERROR_ABORT;
        }
      }
    } break; // ENUMERATIONTYPE_SNAPSHOT

    default: {
      NS_NOTREACHED("Invalid enumeration type");
      rv = NS_ERROR_INVALID_ARG;
    } break;
  }

  aEnumerationListener->OnEnumerationEnd(this, rv);
  return NS_OK;
}

/**
 * See sbIMediaList
 */
NS_IMETHODIMP
sbLocalDatabaseMediaListBase::EnumerateItemsByProperty(const nsAString& aName,
                                                       const nsAString& aValue,
                                                       sbIMediaListEnumerationListener* aEnumerationListener,
                                                       PRUint16 aEnumerationType)
{
  NS_ENSURE_ARG_POINTER(aEnumerationListener);

  // A property name must be specified.
  NS_ENSURE_TRUE(!aName.IsEmpty(), NS_ERROR_INVALID_ARG);

  // Make a single-item string array to hold our property value.
  sbStringArray valueArray(1);
  nsString* value = valueArray.AppendElement();
  NS_ENSURE_TRUE(value, NS_ERROR_OUT_OF_MEMORY);

  value->Assign(aValue);

  // Make a string enumerator for it.
  nsCOMPtr<nsIStringEnumerator> valueEnum =
    new sbTArrayStringEnumerator(&valueArray);
  NS_ENSURE_TRUE(valueEnum, NS_ERROR_OUT_OF_MEMORY);

  nsresult rv;

  switch (aEnumerationType) {

    case sbIMediaList::ENUMERATIONTYPE_LOCKING: {
      NS_ENSURE_TRUE(mFullArrayMonitor, NS_ERROR_FAILURE);
      nsAutoMonitor mon(mFullArrayMonitor);

      // Don't reenter!
      NS_ENSURE_FALSE(mLockedEnumerationActive, NS_ERROR_FAILURE);
      mLockedEnumerationActive = PR_TRUE;

      PRBool beginEnumeration;
      rv = aEnumerationListener->OnEnumerationBegin(this, &beginEnumeration);

      if (NS_SUCCEEDED(rv)) {
        if (beginEnumeration) {
          rv = EnumerateItemsByPropertyInternal(aName, valueEnum,
                                                aEnumerationListener);
        }
        else {
          // The user cancelled the enumeration.
          rv = NS_ERROR_ABORT;
        }
      }

      mLockedEnumerationActive = PR_FALSE;

    } break; // ENUMERATIONTYPE_LOCKING

    case sbIMediaList::ENUMERATIONTYPE_SNAPSHOT: {
      PRBool beginEnumeration;
      rv = aEnumerationListener->OnEnumerationBegin(this, &beginEnumeration);

      if (NS_SUCCEEDED(rv)) {
        if (beginEnumeration) {
          rv = EnumerateItemsByPropertyInternal(aName, valueEnum,
                                                aEnumerationListener);
        }
        else {
          // The user cancelled the enumeration.
          rv = NS_ERROR_ABORT;
        }
      }
    } break; // ENUMERATIONTYPE_SNAPSHOT

    default: {
      NS_NOTREACHED("Invalid enumeration type");
      rv = NS_ERROR_INVALID_ARG;
    } break;
  }

  aEnumerationListener->OnEnumerationEnd(this, rv);
  return NS_OK;
}

/**
 * See sbIMediaList
 */
NS_IMETHODIMP
sbLocalDatabaseMediaListBase::EnumerateItemsByProperties(sbIPropertyArray* aProperties,
                                                         sbIMediaListEnumerationListener* aEnumerationListener,
                                                         PRUint16 aEnumerationType)
{
  NS_ENSURE_ARG_POINTER(aProperties);
  NS_ENSURE_ARG_POINTER(aEnumerationListener);

  PRUint32 propertyCount;
  nsresult rv = aProperties->GetLength(&propertyCount);
  NS_ENSURE_SUCCESS(rv, rv);

  // It doesn't make sense to call this method without specifying any properties
  // so it is probably a caller error if we have none.
  NS_ENSURE_STATE(propertyCount);

  // The guidArray needs AddFilter called only once per property with an
  // enumerator that contains all the values. We were given an array of
  // name/value pairs, so this is a little tricky. We make a hash table that
  // uses the property name for a key and an array of values as its data. Then
  // we load the arrays in a loop and finally call AddFilter as an enumeration
  // function.
  
  sbStringArrayHash propertyHash;
  
  // Init with the propertyCount as the number of buckets to create. This will
  // probably be too many, but it's likely less than the default of 16.
  propertyHash.Init(propertyCount);

  // Load the hash table with properties from the array.
  for (PRUint32 index = 0; index < propertyCount; index++) {

    // Get the property.
    nsCOMPtr<nsIProperty> property;
    rv = aProperties->GetPropertyAt(index, getter_AddRefs(property));
    SB_CONTINUE_IF_FAILED(rv);

    // Get the value.
    nsCOMPtr<nsIVariant> value;
    rv = property->GetValue(getter_AddRefs(value));
    SB_CONTINUE_IF_FAILED(rv);

    // Make sure the value is a string type, otherwise bail.
    PRUint16 dataType;
    rv = value->GetDataType(&dataType);
    SB_CONTINUE_IF_FAILED(rv);

    if (dataType != nsIDataType::VTYPE_ASTRING) {
      NS_WARNING("Wrong data type passed in a properties array!");
      continue;
    }

    // Get the name of the property. This will be the key for the hash table.
    nsAutoString propertyName;
    rv = property->GetName(propertyName);
    SB_CONTINUE_IF_FAILED(rv);

    // Get the string array associated with the key. If it doesn't yet exist
    // then we need to create it.
    sbStringArray* stringArray;
    PRBool arrayExists = propertyHash.Get(propertyName, &stringArray);
    if (!arrayExists) {
      NS_NEWXPCOM(stringArray, sbStringArray);
      SB_CONTINUE_IF_FALSE(stringArray);

      // Try to add the array to the hash table.
      PRBool success = propertyHash.Put(propertyName, stringArray);
      if (!success) {
        NS_WARNING("Failed to add string array to property hash!");
        
        // Make sure to delete the new array, otherwise it will leak.
        NS_DELETEXPCOM(stringArray);
        continue;
      }
    }
    NS_ASSERTION(stringArray, "Must have a valid pointer here!");

    // Now we need a slot for the property value.
    nsString* valueString = stringArray->AppendElement();
    SB_CONTINUE_IF_FALSE(valueString);

    // And finally assign it.
    rv = value->GetAsAString(*valueString);
    SB_CONTINUE_IF_FAILED(rv);
  }

  switch (aEnumerationType) {

    case sbIMediaList::ENUMERATIONTYPE_LOCKING: {
      NS_ENSURE_TRUE(mFullArrayMonitor, NS_ERROR_FAILURE);
      nsAutoMonitor mon(mFullArrayMonitor);

      // Don't reenter!
      NS_ENSURE_FALSE(mLockedEnumerationActive, NS_ERROR_FAILURE);
      mLockedEnumerationActive = PR_TRUE;

      PRBool beginEnumeration;
      rv = aEnumerationListener->OnEnumerationBegin(this, &beginEnumeration);

      if (NS_SUCCEEDED(rv)) {
        if (beginEnumeration) {
          rv = EnumerateItemsByPropertiesInternal(&propertyHash,
                                                  aEnumerationListener);
        }
        else {
          // The user cancelled the enumeration.
          rv = NS_ERROR_ABORT;
        }
      }

      mLockedEnumerationActive = PR_FALSE;

    } break; // ENUMERATIONTYPE_LOCKING

    case sbIMediaList::ENUMERATIONTYPE_SNAPSHOT: {
      PRBool beginEnumeration;
      rv = aEnumerationListener->OnEnumerationBegin(this, &beginEnumeration);

      if (NS_SUCCEEDED(rv)) {
        if (beginEnumeration) {
          rv = EnumerateItemsByPropertiesInternal(&propertyHash,
                                                  aEnumerationListener);
        }
        else {
          // The user cancelled the enumeration.
          rv = NS_ERROR_ABORT;
        }
      }
    } break; // ENUMERATIONTYPE_SNAPSHOT

    default: {
      NS_NOTREACHED("Invalid enumeration type");
      rv = NS_ERROR_INVALID_ARG;
    } break;
  }

  aEnumerationListener->OnEnumerationEnd(this, rv);
  return NS_OK;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::IndexOf(sbIMediaItem* aMediaItem,
                                      PRUint32 aStartFrom,
                                      PRUint32* _retval)
{
  NS_ENSURE_ARG_POINTER(aMediaItem);
  NS_ENSURE_ARG_POINTER(_retval);

  PRUint32 count;

  NS_ENSURE_TRUE(mFullArrayMonitor, NS_ERROR_FAILURE);
  nsAutoMonitor mon(mFullArrayMonitor);

  nsresult rv = mFullArray->GetLength(&count);
  NS_ENSURE_SUCCESS(rv, rv);

  // Do some sanity checking.
  NS_ENSURE_TRUE(count > 0, NS_ERROR_UNEXPECTED);
  NS_ENSURE_ARG_MAX(aStartFrom, count - 1);

  nsAutoString testGUID;
  rv = aMediaItem->GetGuid(testGUID);
  NS_ENSURE_SUCCESS(rv, rv);

  for (PRUint32 index = aStartFrom; index < count; index++) {
    nsAutoString itemGUID;
    rv = mFullArray->GetByIndex(index, itemGUID);
    SB_CONTINUE_IF_FAILED(rv);

    if (testGUID.Equals(itemGUID)) {
      *_retval = index;
      return NS_OK;
    }
  }

  return NS_ERROR_NOT_AVAILABLE;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::LastIndexOf(sbIMediaItem* aMediaItem,
                                          PRUint32 aStartFrom,
                                          PRUint32* _retval)
{
  NS_ENSURE_ARG_POINTER(aMediaItem);
  NS_ENSURE_ARG_POINTER(_retval);

  NS_ENSURE_TRUE(mFullArrayMonitor, NS_ERROR_FAILURE);
  nsAutoMonitor mon(mFullArrayMonitor);

  PRUint32 count;
  nsresult rv = mFullArray->GetLength(&count);
  NS_ENSURE_SUCCESS(rv, rv);

  // Do some sanity checking.
  NS_ENSURE_TRUE(count > 0, NS_ERROR_UNEXPECTED);
  NS_ENSURE_ARG_MAX(aStartFrom, count - 1);

  nsAutoString testGUID;
  rv = aMediaItem->GetGuid(testGUID);
  NS_ENSURE_SUCCESS(rv, rv);

  for (PRUint32 index = count - 1; index >= aStartFrom; index--) {
    nsAutoString itemGUID;
    rv = mFullArray->GetByIndex(index, itemGUID);
    SB_CONTINUE_IF_FAILED(rv);

    if (testGUID.Equals(itemGUID)) {
      *_retval = index;
      return NS_OK;
    }
  }
  return NS_ERROR_NOT_AVAILABLE;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::Contains(sbIMediaItem* aMediaItem,
                                       PRBool* _retval)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetIsEmpty(PRBool* aIsEmpty)
{
  NS_ENSURE_ARG_POINTER(aIsEmpty);

  NS_ENSURE_TRUE(mFullArrayMonitor, NS_ERROR_FAILURE);
  nsAutoMonitor mon(mFullArrayMonitor);

  PRUint32 length;
  nsresult rv = mFullArray->GetLength(&length);
  NS_ENSURE_SUCCESS(rv, rv);

  *aIsEmpty = length == 0;
  return NS_OK;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::Add(sbIMediaItem* aMediaItem)
{
  NS_NOTREACHED("Not meant to be implemented in this base class");
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::AddAll(sbIMediaList* aMediaList)
{
  NS_NOTREACHED("Not meant to be implemented in this base class");
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::AddSome(nsISimpleEnumerator* aMediaItems)
{
  NS_NOTREACHED("Not meant to be implemented in this base class");
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::InsertBefore(PRUint32 aIndex,
                                           sbIMediaItem* aMediaItem)
{
  NS_NOTREACHED("Not meant to be implemented in this base class");
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::MoveBefore(PRUint32 aFromIndex,
                                         PRUint32 aToIndex)
{
  NS_NOTREACHED("Not meant to be implemented in this base class");
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::MoveLast(PRUint32 aIndex)
{
  NS_NOTREACHED("Not meant to be implemented in this base class");
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::Remove(sbIMediaItem* aMediaItem)
{
  NS_NOTREACHED("Not meant to be implemented in this base class");
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::RemoveByIndex(PRUint32 aIndex)
{
  NS_NOTREACHED("Not meant to be implemented in this base class");
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::RemoveSome(nsISimpleEnumerator* aMediaItems)
{
  NS_NOTREACHED("Not meant to be implemented in this base class");
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::Clear()
{
  NS_NOTREACHED("Not meant to be implemented in this base class");
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetTreeView(nsITreeView** aTreeView)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetCascadeFilterSet(sbICascadeFilterSet** aCascadeFilterSet)
{
  nsresult rv;

  if (!mCascadeFilterSet) {
    nsAutoPtr<sbLocalDatabaseCascadeFilterSet> cascadeFilterSet(new sbLocalDatabaseCascadeFilterSet());
    NS_ENSURE_TRUE(cascadeFilterSet, NS_ERROR_OUT_OF_MEMORY);

    nsCOMPtr<sbILocalDatabaseGUIDArray> guidArray;
    rv = mFullArray->Clone(getter_AddRefs(guidArray));
    NS_ENSURE_SUCCESS(rv, rv);

    rv = cascadeFilterSet->Init(this, guidArray);
    NS_ENSURE_SUCCESS(rv, rv);

    mCascadeFilterSet = cascadeFilterSet.forget();
  }

  NS_ADDREF(*aCascadeFilterSet = mCascadeFilterSet);
  return NS_OK;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::AddListener(sbIMediaListListener* aListener)
{
  return sbLocalDatabaseMediaListListener::AddListener(aListener);
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::RemoveListener(sbIMediaListListener* aListener)
{
  return sbLocalDatabaseMediaListListener::RemoveListener(aListener);
}

// sbIMediaItem
NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetLibrary(sbILibrary** aLibrary)
{
  NS_ENSURE_ARG_POINTER(aLibrary);
  NS_ENSURE_STATE(mLibrary);

  nsresult rv;
  nsCOMPtr<sbILibrary> library = do_QueryInterface(mLibrary, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  NS_ADDREF(*aLibrary = library);
  return NS_OK;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetOriginLibrary(sbILibrary** aOriginLibrary)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetIsMutable(PRBool* IsMutable)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetMediaCreated(PRInt32* MediaCreated)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::SetMediaCreated(PRInt32 aMediaCreated)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetMediaUpdated(PRInt32* MediaUpdated)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::SetMediaUpdated(PRInt32 aMediaUpdated)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::TestIsAvailable(nsIObserver* Observer)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetContentSrc(nsIURI** aContentSrc)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::SetContentSrc(nsIURI * aContentSrc)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetContentLength(PRInt32* ContentLength)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::SetContentLength(PRInt32 aContentLength)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetContentType(nsAString& aContentType)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::SetContentType(const nsAString& aContentType)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::OpenInputStream(PRUint32 aOffset,
                                              nsIInputStream** _retval)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::OpenOutputStream(PRUint32 aOffset,
                                               nsIOutputStream** _retval)
{
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::ToString(nsAString& _retval)
{
  nsAutoString buff;

  buff.AppendLiteral("MediaList {guid: ");
  buff.Append(mGuid);
  buff.AppendLiteral("}");

  _retval = buff;
  return NS_OK;
}

// sbILocalDatabaseMediaItem
NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetMediaItemId(PRUint32 *_retval)
{
  if (mMediaItemId == 0) {
    nsresult rv = mLibrary->GetMediaItemIdForGuid(mGuid, &mMediaItemId);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  *_retval = mMediaItemId;
  return NS_OK;
}

// sbIFilterableMediaList
NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetFilterableProperties(nsIStringEnumerator** aFilterableProperties)
{
  NS_ENSURE_ARG_POINTER(aFilterableProperties);

  // Get this from the property manager?
  return NS_ERROR_NOT_IMPLEMENTED;

}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetFilterValues(const nsAString& aPropertyName,
                                              nsIStringEnumerator** _retval)
{
  NS_ENSURE_ARG_POINTER(_retval);

  nsresult rv;
  PRInt32 dbOk;

  nsCOMPtr<sbIDatabaseQuery> query;
  rv = MakeStandardQuery(getter_AddRefs(query));
  NS_ENSURE_SUCCESS(rv, rv);

  rv = query->AddQuery(mDistinctPropertyValuesQuery);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = query->BindStringParameter(0, aPropertyName);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = query->Execute(&dbOk);
  NS_ENSURE_SUCCESS(rv, rv);
  NS_ENSURE_SUCCESS(dbOk, dbOk);

  nsCOMPtr<sbIDatabaseResult> result;
  rv = query->GetResultObject(getter_AddRefs(result));
  NS_ENSURE_SUCCESS(rv, rv);

  sbDatabaseResultStringEnumerator* values =
    new sbDatabaseResultStringEnumerator(result);
  NS_ENSURE_TRUE(values, NS_ERROR_OUT_OF_MEMORY);

  rv = values->Init();
  NS_ENSURE_SUCCESS(rv, rv);

  NS_ADDREF(*_retval = values);
  return NS_OK;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::SetFilters(sbIPropertyArray* aPropertyArray)
{
  NS_ENSURE_ARG_POINTER(aPropertyArray);

  NS_ENSURE_TRUE(mViewArrayLock, NS_ERROR_FAILURE);
  nsAutoLock lock(mViewArrayLock);

  nsresult rv = UpdateFiltersInternal(aPropertyArray, PR_TRUE);
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::UpdateFilters(sbIPropertyArray* aPropertyArray)
{
  NS_ENSURE_ARG_POINTER(aPropertyArray);

  NS_ENSURE_TRUE(mViewArrayLock, NS_ERROR_FAILURE);
  nsAutoLock lock(mViewArrayLock);

  nsresult rv = UpdateFiltersInternal(aPropertyArray, PR_FALSE);
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::RemoveFilters(sbIPropertyArray* aPropertyArray)
{
  NS_ENSURE_ARG_POINTER(aPropertyArray);

  nsresult rv;

  NS_ENSURE_TRUE(mViewArrayLock, NS_ERROR_FAILURE);
  nsAutoLock lock(mViewArrayLock);

  PRUint32 propertyCount;
  rv = aPropertyArray->GetLength(&propertyCount);
  NS_ENSURE_SUCCESS(rv, rv);

  // ensure we got something
  NS_ENSURE_STATE(propertyCount);

  PRBool dirty = PR_FALSE;
  for (PRUint32 index = 0; index < propertyCount; index++) {

    // Get the property.
    nsCOMPtr<nsIProperty> property;
    rv = aPropertyArray->GetPropertyAt(index, getter_AddRefs(property));
    NS_ENSURE_SUCCESS(rv, rv);

    // Get the value.
    nsCOMPtr<nsIVariant> value;
    rv = property->GetValue(getter_AddRefs(value));
    NS_ENSURE_SUCCESS(rv, rv);

    // Make sure the value is a string type, otherwise bail.
    PRUint16 dataType;
    rv = value->GetDataType(&dataType);
    NS_ENSURE_SUCCESS(rv, rv);

    NS_ENSURE_TRUE(dataType == nsIDataType::VTYPE_ASTRING,
                   NS_ERROR_INVALID_ARG);

    // Get the name of the property. This will be the key for the hash table.
    nsAutoString propertyName;
    rv = property->GetName(propertyName);
    NS_ENSURE_SUCCESS(rv, rv);

    // Get the property value
    nsAutoString valueString;
    rv = value->GetAsAString(valueString);
    NS_ENSURE_SUCCESS(rv, rv);

    sbStringArray* stringArray;
    PRBool arrayExists = mViewFilters.Get(propertyName, &stringArray);
    // If there is an array for this property, search the array for the value
    // and remove it
    if (arrayExists) {
      PRUint32 length = stringArray->Length();
      // Do this backwards so we don't have to deal with the array shifting
      // on us.  Also, be sure to remove multiple copies of the same string.
      for (PRInt32 i = length - 1; i >= 0; i--) {
        if (stringArray->ElementAt(i).Equals(valueString)) {
          stringArray->RemoveElementAt(i);
          dirty = PR_TRUE;
        }
      }
    }
  }

  if (dirty) {
    rv = UpdateViewArrayConfiguration();
    NS_ENSURE_SUCCESS(rv, rv);
  }

  return NS_OK;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::ClearFilters()
{
  NS_ENSURE_TRUE(mViewArrayLock, NS_ERROR_FAILURE);
  nsAutoLock lock(mViewArrayLock);

  mViewFilters.Clear();

  nsresult rv = UpdateViewArrayConfiguration();
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

// sbISearchableMediaList
NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetSearchableProperties(nsIStringEnumerator** aSearchableProperties)
{
  NS_ENSURE_ARG_POINTER(aSearchableProperties);
  // To be implemented by property manager?
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetCurrentSearch(sbIPropertyArray** aCurrentSearch)
{
  NS_ENSURE_ARG_POINTER(aCurrentSearch);

  nsresult rv;

  NS_ENSURE_TRUE(mViewArrayLock, NS_ERROR_FAILURE);
  nsAutoLock lock(mViewArrayLock);

  if (!mViewSearches) {
    mViewSearches = do_CreateInstance(SB_PROPERTYARRAY_CONTRACTID, &rv);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  NS_ADDREF(*aCurrentSearch = mViewSearches);
  return NS_OK;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::SetSearch(sbIPropertyArray* aSearch)
{
  NS_ENSURE_ARG_POINTER(aSearch);

  nsresult rv;

  NS_ENSURE_TRUE(mViewArrayLock, NS_ERROR_FAILURE);
  nsAutoLock lock(mViewArrayLock);

  mViewSearches = aSearch;

  rv = UpdateViewArrayConfiguration();
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::ClearSearch()
{
  nsresult rv;

  NS_ENSURE_TRUE(mViewArrayLock, NS_ERROR_FAILURE);
  nsAutoLock lock(mViewArrayLock);

  if (mViewSearches) {
    rv = mViewSearches->Clear();
    NS_ENSURE_SUCCESS(rv, rv);
  }

  rv = UpdateViewArrayConfiguration();
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

// sbISortableMediaList
NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetSortableProperties(nsIStringEnumerator** aSortableProperties)
{
  NS_ENSURE_ARG_POINTER(aSortableProperties);
  // To be implemented by property manager?
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetCurrentSort(sbIPropertyArray** aCurrentSort)
{
  NS_ENSURE_ARG_POINTER(aCurrentSort);

  nsresult rv;

  NS_ENSURE_TRUE(mViewArrayLock, NS_ERROR_FAILURE);
  nsAutoLock lock(mViewArrayLock);

  if (!mViewSort) {
    mViewSort = do_CreateInstance(SB_PROPERTYARRAY_CONTRACTID, &rv);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  NS_ADDREF(*aCurrentSort = mViewSort);

  return NS_OK;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::SetSort(sbIPropertyArray* aSort)
{
  NS_ENSURE_ARG_POINTER(aSort);

  nsresult rv;

  NS_ENSURE_TRUE(mViewArrayLock, NS_ERROR_FAILURE);
  nsAutoLock lock(mViewArrayLock);

  mViewSort = aSort;

  rv = UpdateViewArrayConfiguration();
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::ClearSort()
{
  nsresult rv;

  NS_ENSURE_TRUE(mViewArrayLock, NS_ERROR_FAILURE);
  nsAutoLock lock(mViewArrayLock);

  if (mViewSort) {
    rv = mViewSort->Clear();
    NS_ENSURE_SUCCESS(rv, rv);
  }

  rv = UpdateViewArrayConfiguration();
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

/**
 * \brief Updates the internal filter map with a contents of a property bag.
 *        In replace mode, the value list for each distinct property in the
 *        bag is first cleared before the values in the bag are added.  When
 *        not in replace mode, the new values in the bag are simply appended
 *        to each property's list of values.
 */
nsresult
sbLocalDatabaseMediaListBase::UpdateFiltersInternal(sbIPropertyArray* aPropertyArray,
                                                    PRBool aReplace)
{
  nsresult rv;

  PRUint32 propertyCount;
  rv = aPropertyArray->GetLength(&propertyCount);
  NS_ENSURE_SUCCESS(rv, rv);

  // ensure we got something
  NS_ENSURE_STATE(propertyCount);

  nsTHashtable<nsStringHashKey> seenProperties;
  PRBool success = seenProperties.Init();
  NS_ENSURE_TRUE(success, NS_ERROR_OUT_OF_MEMORY);

  for (PRUint32 index = 0; index < propertyCount; index++) {

    // Get the property.
    nsCOMPtr<nsIProperty> property;
    rv = aPropertyArray->GetPropertyAt(index, getter_AddRefs(property));
    NS_ENSURE_SUCCESS(rv, rv);

    // Get the value.
    nsCOMPtr<nsIVariant> value;
    rv = property->GetValue(getter_AddRefs(value));
    NS_ENSURE_SUCCESS(rv, rv);

    // Make sure the value is a string type, otherwise bail.
    PRUint16 dataType;
    rv = value->GetDataType(&dataType);
    NS_ENSURE_SUCCESS(rv, rv);

    NS_ENSURE_TRUE(dataType == nsIDataType::VTYPE_ASTRING,
                   NS_ERROR_INVALID_ARG);

    // Get the name of the property. This will be the key for the hash table.
    nsAutoString propertyName;
    rv = property->GetName(propertyName);
    NS_ENSURE_SUCCESS(rv, rv);

    // Get the property value
    nsAutoString valueString;
    rv = value->GetAsAString(valueString);
    NS_ENSURE_SUCCESS(rv, rv);

    // If the string is empty and we are replacing, we should delete the
    // property from the hash
    if (valueString.IsEmpty()) {
      if (aReplace) {
        mViewFilters.Remove(propertyName);
      }
    }
    else {
      // Get the string array associated with the key. If it doesn't yet exist
      // then we need to create it.
      sbStringArray* stringArray;
      PRBool arrayExists = mViewFilters.Get(propertyName, &stringArray);

      if (!arrayExists) {
        NS_NEWXPCOM(stringArray, sbStringArray);
        NS_ENSURE_TRUE(stringArray, NS_ERROR_OUT_OF_MEMORY);

        // Try to add the array to the hash table.
        success = mViewFilters.Put(propertyName, stringArray);
        NS_ENSURE_TRUE(success, NS_ERROR_OUT_OF_MEMORY);
      }

      if (aReplace) {
        // If this is the first time we've seen this property, clear the
        // string array
        if (!seenProperties.GetEntry(propertyName)) {
          stringArray->Clear();
          nsStringHashKey* successKey = seenProperties.PutEntry(propertyName);
          NS_ENSURE_TRUE(successKey, NS_ERROR_OUT_OF_MEMORY);
        }
      }

      // Now we need a slot for the property value.
      nsString* valueStringPtr = stringArray->AppendElement();
      NS_ENSURE_TRUE(valueStringPtr, NS_ERROR_OUT_OF_MEMORY);

      valueStringPtr->Assign(valueString);
    }
  }

  rv = UpdateViewArrayConfiguration();
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

nsresult
sbLocalDatabaseMediaListBase::UpdateViewArrayConfiguration()
{
  nsresult rv;

  // Update filters
  rv = mViewArray->ClearFilters();
  NS_ENSURE_SUCCESS(rv, rv);

  mViewFilters.EnumerateRead(AddFilterToGUIDArrayCallback, mViewArray);

  // Update searches
  if (mViewSearches) {
    PRUint32 propertyCount;
    rv = mViewSearches->GetLength(&propertyCount);
    NS_ENSURE_SUCCESS(rv, rv);

    for (PRUint32 index = 0; index < propertyCount; index++) {

      nsCOMPtr<nsIProperty> property;
      rv = mViewSearches->GetPropertyAt(index, getter_AddRefs(property));
      NS_ENSURE_SUCCESS(rv, rv);

      nsCOMPtr<nsIVariant> value;
      rv = property->GetValue(getter_AddRefs(value));
      NS_ENSURE_SUCCESS(rv, rv);

      nsAutoString propertyName;
      rv = property->GetName(propertyName);
      NS_ENSURE_SUCCESS(rv, rv);

      nsAutoString stringValue;
      rv = value->GetAsAString(stringValue);
      NS_ENSURE_SUCCESS(rv, rv);

      sbStringArray valueArray(1);
      nsString* successString = valueArray.AppendElement(stringValue);
      NS_ENSURE_TRUE(successString, NS_ERROR_OUT_OF_MEMORY);

      nsCOMPtr<nsIStringEnumerator> valueEnum =
        new sbTArrayStringEnumerator(&valueArray);
      NS_ENSURE_TRUE(valueEnum, NS_ERROR_OUT_OF_MEMORY);

      rv = mViewArray->AddFilter(propertyName, valueEnum, PR_TRUE);
      NS_ENSURE_SUCCESS(rv, rv);
    }
  }

  // Update sort
  rv = mViewArray->ClearSorts();
  NS_ENSURE_SUCCESS(rv, rv);

  PRBool hasSorts = PR_FALSE;
  if (mViewSort) {
    PRUint32 propertyCount;
    rv = mViewSort->GetLength(&propertyCount);
    NS_ENSURE_SUCCESS(rv, rv);

    for (PRUint32 index = 0; index < propertyCount; index++) {

      nsCOMPtr<nsIProperty> property;
      rv = mViewSort->GetPropertyAt(index, getter_AddRefs(property));
      NS_ENSURE_SUCCESS(rv, rv);

      nsCOMPtr<nsIVariant> value;
      rv = property->GetValue(getter_AddRefs(value));
      NS_ENSURE_SUCCESS(rv, rv);

      nsAutoString propertyName;
      rv = property->GetName(propertyName);
      NS_ENSURE_SUCCESS(rv, rv);

      nsAutoString stringValue;
      rv = value->GetAsAString(stringValue);
      NS_ENSURE_SUCCESS(rv, rv);

      mViewArray->AddSort(propertyName, stringValue.EqualsLiteral("a"));
      NS_ENSURE_SUCCESS(rv, rv);

      hasSorts = PR_TRUE;
    }
  }

  // If no sport is specified, use the default sort
  if (!hasSorts) {
    nsAutoString defaultProperty;
    rv = GetDefaultSortProperty(defaultProperty);
    NS_ENSURE_SUCCESS(rv, rv);

    mViewArray->AddSort(defaultProperty, PR_TRUE);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  // Invalidate the view array
  rv = mViewArray->Invalidate();
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

// nsIClassInfo
NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetInterfaces(PRUint32* count, nsIID*** array)
{
  return NS_CI_INTERFACE_GETTER_NAME(sbLocalDatabaseMediaListBase)(count, array);
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetHelperForLanguage(PRUint32 language,
                                                   nsISupports** _retval)
{
  *_retval = nsnull;
  return NS_OK;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetContractID(char** aContractID)
{
  *aContractID = nsnull;
  return NS_OK;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetClassDescription(char** aClassDescription)
{
  *aClassDescription = nsnull;
  return NS_OK;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetClassID(nsCID** aClassID)
{
  *aClassID = nsnull;
  return NS_OK;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetImplementationLanguage(PRUint32* aImplementationLanguage)
{
  *aImplementationLanguage = nsIProgrammingLanguage::CPLUSPLUS;
  return NS_OK;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetFlags(PRUint32 *aFlags)
{
// not yet  *aFlags = nsIClassInfo::THREADSAFE;
  *aFlags = 0;
  return NS_OK;
}

NS_IMETHODIMP
sbLocalDatabaseMediaListBase::GetClassIDNoAlloc(nsCID* aClassIDNoAlloc)
{
  return NS_ERROR_NOT_AVAILABLE;
}

NS_IMPL_ISUPPORTS1(sbDatabaseResultStringEnumerator, nsIStringEnumerator)

// nsIStringEnumerator
nsresult
sbDatabaseResultStringEnumerator::Init()
{
  nsresult rv = mDatabaseResult->GetRowCount(&mLength);
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

NS_IMETHODIMP
sbDatabaseResultStringEnumerator::HasMore(PRBool *_retval)
{
  *_retval = mNextIndex < mLength;
  return NS_OK;
}

NS_IMETHODIMP
sbDatabaseResultStringEnumerator::GetNext(nsAString& _retval)
{
  nsresult rv = mDatabaseResult->GetRowCell(mNextIndex, 0, _retval);
  NS_ENSURE_SUCCESS(rv, rv);
  mNextIndex++;

  return NS_OK;
}

