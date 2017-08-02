//
//  ClassWithSomething.h
//  Sample
//
//

#import <Foundation/Foundation.h>

@protocol SomeDelegate<NSObject>
@property NSMutableDictionary* aMutableDictionary;
- (void)doSomething;
- (void)doSomething:(NSNumber*)smth Else:(NSString*)Else;
@end

@protocol SomeOtherDelegate<NSObject>
- (void)doSomethingOnOtherDelegate:(NSNumber*)points;
@end

typedef struct SomeStruct
{
	struct SomeStruct* next;
	int value;
} SomeStruct;

@interface TheBaseOfTheMixinWithSomething: NSObject

@property (readonly) NSValue* anyValue;

@end

@interface MixinWithSomething : TheBaseOfTheMixinWithSomething

- (instancetype)initWithName:(NSString*)name;

- (NSString*)invertName;

- (NSString*)concatenateWithPrefix:(NSString*)prefix
                             suffix:(NSString*)suffix;

- (SomeStruct)buildSomeStruct:(int)flag;

@property (readwrite) NSNumber* length;

@property (class) NSString* aClassProperty;

+ (NSInteger)someClassMethodWithString:(NSString*)s andArray:(NSArray*)array;

@end

@interface MixinWithSomething (SomeWeirdExtension)

+ (BOOL) staticMemberForRescue:(MixinWithSomething*)parent;

@end
